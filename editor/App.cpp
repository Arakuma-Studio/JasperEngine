// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "App.h"
#include "render/ViewCameraMirror.h"
#include "component/Transform.h"
#include "component/CameraShakerComponent.h"
#include "pool/ShaderPool.h"
#include "render/SceneRender.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "window/AnimationWindow.h"
#include "window/AnimatorWindow.h"
#include "window/AssetPanels.h"
#include "window/AudioMixerWindow.h"
#include "window/ParticleEffectWindow.h"
#include "window/AudioCapturesWindow.h"
#include "window/SoundBrowserWindow.h"
#include "window/GraphEditorWindow.h"
#include "window/AvatarWindow.h"
#include "window/CodeEditor.h"
#include "window/GameWindow.h"
#include "window/HierarchyWindow.h"
#include "window/ImageViewerWindow.h"
#include "window/InspectorWindow.h"
#include "window/LoadingWindow.h"
#include "window/MainToolbar.h"
#include "window/PhysicsPlacementWindow.h"
#include "window/PlayInput.h"
#include "window/PoseAnimationWindow.h"
#include "window/ProjectWindow.h"
#include "window/SceneWindow.h"
#include "window/SpriteNormalsWindow.h"
#include "window/StatusBar.h"
#include "window/SystemsWindow.h"
#include "window/TerrainEditWindow.h"
#include "window/TextureSynthesisWindow.h"
#include "window/VertexAnimationBakerWindow.h"
#include "component/TilemapComponent.h"
#include "spline/SplineSceneTools.h"
#include "spline/SplineEditor.h"
#include "particles/ParticleEffectLibrary.h"
#include "util/ParticleMigrationReport.h"
#include "AppMenuCommand.h"
#include "util/NumericExpression.h"
#include "util/EditorFingerprint.h"
#include "util/ScriptReloadService.h"
#include "util/ScriptStatus.h"
#include "util/EditorScripts.h"
#include "profiler/Profile.h"
#include "asset/AssetRefEditor.h"
#include "util/Scheduler.h"
#include "window/AnimationWindowClip.h"
#include "thread/ResourceProgress.h"

#include "imgui_internal.h"
#include "Platform.h"
#include "Backend.h"
#include "manager/SceneManager.h"
#include "Engine.h"
#include "device/Device.h"
#include "cli/CommandLine.h"
#include "render/SystemRender.h"

#include "external/IconsFontAwesome6.h"
#include "audio/AudioMixerDocument.h"
#include "audio/LoudnessMonitor.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "lattice/LatticeSceneTools.h"
#include "navigation/NavigationEditor.h"
#include "util/EntityClipboard.h"
#include "mcp/McpActivityWindow.h"
#include "window/JobsMemoryWindow.h"
#include "window/MeshAuditWindow.h"
#include "window/BuildReportWindow.h"
#include "window/ImporterFields.h"
#include "window/profiler/FrameDebuggerWindow.h"
#include "window/profiler/ProfilerWindow.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profiler.h"
#include "window/PlayModeChangesWindow.h"
#include "window/UndoHistoryWindow.h"
#include "window/vcs/VcsUi.h"
#include "window/InputActionsWindow.h"
#include "navigation/NavigationWindow.h"
#include "command/PlayChangeReview.h"
#include "pose/PoseEditSession.h"
#include "widget/ColorPickerWindow.h"
#include "util/GraphicUtils.h"
#include "recorder/RecorderSession.h"
#include "hub/HubPaths.h"
#include "platform/LaunchKeys.h"
#include "io/AtomicFile.h"
#include "widget/CurveField.h"
#include "widget/GradientField.h"
#include "widget/FieldEdit.h"
#include "mcp/McpServer.h"
#include "window/EditorFilePicker.h"
#include "window/EditorModal.h"
#include "mcp/McpAutoSync.h"
#include "mcp/tools/McpTools.h"
#include "reload/HotReloadService.h"
#include "search/SearchPalette.h"
#include "shortcuts/DefaultCommands.h"
#include "shortcuts/PanelClaims.h"
#include "shortcuts/ShortcutsWindow.h"
#include "window/BookmarksWindow.h"
#include "texture/TextureCookQueue.h"
#include "model/ModelCookQueue.h"

#include "util/CrashHandler.h"
#include "util/EditorDiagnostics.h"
#include "diag/HangWatchdog.h"
#include "util/FileDialogs.h"
#include "util/ProjectUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "EditorVersion.h"
#include "Out.h"
#include "window/HumanoidConvertDialog.h"
#include "AppSettings.h"
#include "Theme.h"
#include "resources/fonts/fa-solid-900_ttf.h"
#include "resources/fonts/jetbrains-mono-regular_ttf.h"
#include "resources/fonts/roboto-latin-bold_ttf.h"
#include "util/DefaultFont.h"
#include "util/DefaultFontArabic.h"

#include "shader/ShaderBuilder.h"
#include "htmlui/effects/EffectRegistry.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/HtmlUiAutoImport.h"
#include "render/preview/PoseRender.h"
#include "render/preview/VatPreviewRender.h"
#include "particles/ParticleEffectPreview.h"
#include "procmat/ProceduralRun.h"
#include "util/FileUtils.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"
#include "pose/PoseTemplates.h"
#include "pose/PoseViewport.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "unity/UnityParticleImport.h"

#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cstdio>

#if defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
#endif

using namespace jasperengine;

namespace {

// A scale no monitor reports means the value is missing or corrupt.
float sanitizeUiScale(float scale) {
    return (scale >= 0.5f && scale <= 8.0f) ? scale : 1.0f;
}

// Saved sizes are physical pixels, so they only mean the same window again at
// the scale they were captured at. Settings from before the editor tracked DPI
// carry no scale and read as 1.0, which is what a DPI-unaware process reported.
int scaleSavedWindowSize(int saved, float uiScale) {
    const float savedScale = sanitizeUiScale(editor::AppSettings::getWindowUiScale());
    const float ratio = sanitizeUiScale(uiScale) / savedScale;
    return std::max(1, static_cast<int>(std::lround(saved * ratio)));
}

// The dockspace lives on the main viewport, so that is the scale it has to match,
// times the UI Scale of Editor Settings.
float mainViewportScale() {
    return sanitizeUiScale(ImGui::GetMainViewport()->DpiScale) * editor::Theme::uiScale();
}

// DPI times UI scale, so a wider range than sanitizeUiScale().
float sanitizeLayoutScale(float scale) {
    return (scale >= 0.25f && scale <= 24.0f) ? scale : 1.0f;
}

// Share of the window a panel beside the scene may take (buildDefaultLayout()
// and scaleDockChildren()).
constexpr float panelMaxShare = 0.45f;

// HasCentralNodeChild is not set yet when a restored layout is rescaled.
bool holdsCentralNode(const ImGuiDockNode* node) {
    if (!node) return false;
    if (node->LocalFlags & ImGuiDockNodeFlags_CentralNode) return true;
    return holdsCentralNode(node->ChildNodes[0]) || holdsCentralNode(node->ChildNodes[1]);
}

}

ImFont* editor::App::codeFont = nullptr;
ImFont* editor::App::boldFont = nullptr;

editor::App::App(){
    scheduler().setOwnerThread();
    mainThreadId = std::this_thread::get_id();
    inspectorWindow = new InspectorWindow(&project);
    consoleWindow = new ConsoleWindow();
    playInput = new PlayInput(&project);
    sceneWindow = new SceneWindow(&project);
    gameWindow = new GameWindow(&project, playInput);
    // the open project's files changing on disk reload (S05)
    project.setWatchFiles(true);
    mainToolbar = new MainToolbar(&project);
    statusBar = new StatusBar([this]() {
        editorSettingsWindow.open(&project);
        editorSettingsWindow.showPage("Asset Pipeline");
    });
    inspectorWindow->setSceneWindow(sceneWindow);
    hierarchyWindow = new HierarchyWindow(&project, sceneWindow);
    codeEditor = new CodeEditor(&project);
    imageViewerWindow = new ImageViewerWindow(&project);
    projectWindow = new ProjectWindow(&project, codeEditor, imageViewerWindow);
    loadingWindow = new LoadingWindow();
    animationWindow = new AnimationWindow(&project);
    poseAnimationWindow = new PoseAnimationWindow(&project);
    animatorWindow = new AnimatorWindow(&project, poseAnimationWindow);
    avatarWindow = new AvatarWindow(&project);
    terrainEditWindow = new TerrainEditWindow(&project);
    textureSynthesisWindow = new TextureSynthesisWindow(&project);
    audioMixerWindow = new AudioMixerWindow(&project);
    particleEffectWindow = new ParticleEffectWindow(&project);
    soundBrowserWindow = new SoundBrowserWindow(&project);
    graphEditorWindow = new GraphEditorWindow(&project);
    audioCapturesWindow = new AudioCapturesWindow(&project);
    vertexAnimationBakerWindow = new VertexAnimationBakerWindow(&project);
    physicsPlacementWindow = new PhysicsPlacementWindow(&project);
    spriteNormalsWindow = new SpriteNormalsWindow(&project);
    spriteNormalsWatcher = new SpriteNormalsWatcher(&project);
    SpriteNormalsWatcher::setActive(spriteNormalsWatcher);
    mcpActivityWindow = new mcp::McpActivityWindow();
    systemsWindow = new SystemsWindow(&project);
    missingAssetsWindow = new MissingAssetsWindow(&project);
    assetReferencesWindow = new AssetReferencesWindow(&project);
    jobsMemoryWindow = new JobsMemoryWindow();
    meshAuditWindow = new MeshAuditWindow(&project);
    buildReportWindow = new BuildReportWindow(&project);
    profilerWindow = new ProfilerWindow(this);
    frameDebuggerWindow = new FrameDebuggerWindow(this);
    undoHistoryWindow = new UndoHistoryWindow(&project);
    vcsUi = new vcs::VcsUi(&project);
    StatusBar::setVersionControlSegment([this]() { vcsUi->drawStatusBarSegment(); });
    inputActionsWindow = new InputActionsWindow(&project);
    navigationWindow = new NavigationWindow(&project);
    playModeChangesWindow = new PlayModeChangesWindow(&project);
    searchPalette = new SearchPalette(this);
    shortcutsWindow = new ShortcutsWindow(CommandCatalog::editor(), shortcutDispatcher);
    // Ctrl+S while playing, and any key whose command cannot run now: the status bar says why
    shortcutDispatcher.setDisabledHandler([](const CommandDef& def, const std::string& reason) {
        StatusBar::setTransientMessage(def.name + ": " + reason, 4.0);
    });
    bookmarksWindow = new BookmarksWindow(&project);
    // Stop found edits that can be kept: the window opens, or after an agent's Stop the
    // Console says where they are (S24 R30)
    PlayChangeReview::get().onPending = [this](uint32_t sceneId, size_t rows) {
        if (PlayChangeReview::get().isQuietStop()) {
            Out::info("%zu play-mode change%s can be kept: Window > General > Play Mode Changes", rows, rows == 1 ? "" : "s");
            return;
        }
        playModeChangesWindow->openFor(sceneId);
    };
    // pose templates saved or deleted (and their undo/redo): the asset database and the Project
    // window see the files (MCP writes templates through the pose document, which syncs itself)
    PoseTemplates::setFilesChangedHook([this](const std::vector<std::filesystem::path>&){
        project.refreshAssetDatabase(true);
        projectWindow->refreshCurrentDirectory();
    });

    isInitialized = false;
    dockspaceNeedsRebuild = false;

    lastFocusedWindow = LastFocusedWindow::None;

    Out::setConsoleWindow(consoleWindow);

    isDroppedExternalPaths = false;

    resetLastActivatedScene();
}

void editor::App::saveFunc(){
    if (saveDialogInProgress) return;
    if (project.isTempProject()) {
        saveAllFunc();
        return;
    }
    if (lastFocusedWindow == LastFocusedWindow::Code) {
        if (!codeEditor->saveLastFocused()) {
            registerAlert("Save Failed", "The script could not be saved. Check its file permissions and try again.");
        }
    }else if (!project.isAnyScenePlaying()) {
        project.saveLastSelectedScene();
    }
}

void editor::App::saveAllFunc(std::function<void(bool)> callback, bool includeUntitled){
    if (project.isAnyScenePlaying() || saveDialogInProgress) {
        if (callback) callback(false);
        return;
    }
    codeEditor->saveAll();
    if (codeEditor->hasUnsavedChanges()) {
        registerAlert("Save Failed", "Some scripts could not be saved. Check their file permissions and try again.");
        if (callback) callback(false);
        return;
    }
    project.saveAllScenes([this, callback](bool success) {
        if (!success) {
            if (callback) callback(false);
            return;
        }
        if (project.isTempProject()) {
            registerProjectSaveDialog([callback](bool saved) { if (callback) callback(saved); });
        } else {
            const bool saved = project.saveProject(true);
            if (!saved) registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");
            if (callback) callback(saved);
        }
    }, includeUntitled);
}

void editor::App::saveAllAndProject(std::function<void()> onSuccess){
    saveAllFunc([onSuccess](bool success) {
        if (success && onSuccess) onSuccess();
    });
}

void editor::App::openProjectFunc(){
    if (project.isAnyScenePlaying()) {
        registerAlert("Scene Running", "A scene is currently running or stopping. Stop it before opening another project.");
        return;
    }

    // the Project Hub's Open page (S27 R17)
    auto openProject = [this]() { openProjectHub(ProjectHubWindow::Page::Open); };
    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        registerConfirmAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before opening another project?",
            [this, openProject]() { saveAllAndProject(openProject); },
            openProject);
    } else {
        openProject();
    }
}

namespace {

using editor::AppMenuCommand;

editor::PlatformMenuItem menuCommand(AppMenuCommand command,
                                     std::string label,
                                     bool enabled = true) {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Command;
    item.label = std::move(label);
    item.command.id = static_cast<uint32_t>(command);
    item.enabled = enabled;
    if (command == AppMenuCommand::About) item.role = editor::PlatformMenuRole::About;
    if (command == AppMenuCommand::EditorSettings) item.role = editor::PlatformMenuRole::Settings;
    if (command == AppMenuCommand::Exit) item.role = editor::PlatformMenuRole::Quit;
    return item;
}

// The menu item of a catalog command: its AppMenuCommand and payload, its shortcut text (the
// user's binding) and its enabled state, under `label` (the menu's wording) or its name
editor::PlatformMenuItem catalogItem(const char* id, std::string label = {}, bool checked = false) {
    const editor::CommandCatalog& catalog = editor::CommandCatalog::editor();
    const editor::CommandDef* def = catalog.find(id);
    editor::PlatformMenuItem item;
    if (!def || !def->menuCommand) return item;
    item = menuCommand(static_cast<AppMenuCommand>(*def->menuCommand), label.empty() ? def->name : std::move(label),
                       !def->enabled || def->enabled());
    if (!item.enabled && def->disabledReason) item.tooltip = def->disabledReason();
    item.command.payload = def->menuPayload;
    item.shortcut = catalog.menuShortcutText(editor::PanelClaims::editor().shownCommand(id));
    item.checked = checked;
    return item;
}

editor::PlatformMenuItem menuPayloadCommand(AppMenuCommand command,
                                            std::string label,
                                            std::string payload,
                                            bool enabled = true) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label), enabled);
    item.command.payload = std::move(payload);
    return item;
}

editor::PlatformMenuItem menuSeparator() {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Separator;
    return item;
}

editor::PlatformMenuItem menuSubmenu(std::string label,
                                     std::vector<editor::PlatformMenuItem> children,
                                     bool enabled = true) {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Submenu;
    item.label = std::move(label);
    item.enabled = enabled;
    item.children = std::move(children);
    return item;
}

// Window titles may carry an ImGui "###id" suffix, which native menus would show.
std::string windowMenuLabel(const char* windowName) {
    std::string label = windowName;
    const size_t idPos = label.find("##");
    if (idPos != std::string::npos) label.resize(idPos);
    return label;
}

// GameObject menu from the Hierarchy's create table (HierarchyWindow::getCreateMenuEntries),
// in the same order and grouping the Hierarchy's own menus use. Each entry is a catalog command.
std::vector<editor::PlatformMenuItem> gameObjectMenuItems(editor::SceneProject* scene, bool canCreate) {
    using editor::HierarchyWindow;
    using editor::CreateMenuEntry;
    const auto& entries = HierarchyWindow::getCreateMenuEntries();
    auto offered = [scene](const CreateMenuEntry& entry) {
        return !scene || HierarchyWindow::isCreateEntryAvailable(entry, scene->sceneType);
    };
    auto entryItem = [&](size_t index) {
        return catalogItem(editor::createEntryCommandId(index).c_str(), entries[index].label);
    };

    std::vector<editor::PlatformMenuItem> items;
    std::vector<std::string> shownSubmenus;
    int lastKind = -1; // 0 = item, 1 = submenu
    for (size_t i = 0; i < entries.size(); i++) {
        const CreateMenuEntry& entry = entries[i];
        if (!offered(entry)) continue;
        if (!entry.submenu) {
            if (lastKind == 1) items.push_back(menuSeparator());
            lastKind = 0;
            items.push_back(entryItem(i));
            // Unity lists Create Empty Child right under Create Empty
            if (entry.type == editor::EntityCreationType::GAMEOBJECT) {
                items.push_back(catalogItem("gameobject.createEmptyChild"));
            }
            continue;
        }
        const std::string submenu = entry.submenu;
        if (std::find(shownSubmenus.begin(), shownSubmenus.end(), submenu) != shownSubmenus.end()) continue;
        shownSubmenus.push_back(submenu);
        if (lastKind == 0) items.push_back(menuSeparator());
        lastKind = 1;

        std::vector<editor::PlatformMenuItem> children;
        for (size_t j = i; j < entries.size(); j++) {
            const CreateMenuEntry& child = entries[j];
            if (!child.submenu || submenu != child.submenu || !offered(child)) continue;
            if (child.separatorBefore && !children.empty()) children.push_back(menuSeparator());
            children.push_back(entryItem(j));
        }
        items.push_back(menuSubmenu(submenu, std::move(children), canCreate));
    }
    return items;
}

} // namespace

namespace {

    // An editor script's item at its path in a menu's items (submenus made as needed), after a
    // separator from the editor's own items
    void insertScriptMenuItem(std::vector<editor::PlatformMenuItem>& items, const std::vector<std::string>& parts, size_t at,
                              const editor::EditorMenuItem& script, bool& separated) {
        if (at + 1 >= parts.size()) {
            if (!separated && !items.empty() && items.back().type != editor::PlatformMenuItemType::Separator) items.push_back(menuSeparator());
            separated = true;
            editor::PlatformMenuItem item = menuPayloadCommand(AppMenuCommand::ScriptMenu, parts.back(), script.id,
                                                               editor::EditorScripts::get().isEnabled(script));
            // the annotation's shortcut is the default of its catalog command (S23 R42), rebindable
            item.shortcut = editor::CommandCatalog::editor().menuShortcutText("script." + script.path);
            items.push_back(std::move(item));
            return;
        }
        for (editor::PlatformMenuItem& item : items) {
            if (item.type == editor::PlatformMenuItemType::Submenu && item.label == parts[at]) {
                bool inner = true;
                insertScriptMenuItem(item.children, parts, at + 1, script, inner);
                return;
            }
        }
        if (!separated && !items.empty() && items.back().type != editor::PlatformMenuItemType::Separator) items.push_back(menuSeparator());
        separated = true;
        std::vector<editor::PlatformMenuItem> children;
        bool inner = true;
        insertScriptMenuItem(children, parts, at + 1, script, inner);
        items.push_back(menuSubmenu(parts[at], std::move(children)));
    }

    std::vector<std::string> splitMenuPath(const std::string& path) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!part.empty()) parts.push_back(part);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return parts;
    }

}

// The project's editor-script menu items (Tools/..., GameObject/..., Assets/...) in the main
// menu: an existing top-level menu, or a new one before Window
void editor::App::addScriptMenus(PlatformMenuModel& menu){
    std::map<std::string, bool> separated;
    for (const EditorMenuItem* script : EditorScripts::get().itemsFor("main")) {
        const std::vector<std::string> parts = splitMenuPath(script->path);
        if (parts.size() < 2) continue;
        auto top = std::find_if(menu.menus.begin(), menu.menus.end(), [&](const PlatformMenuItem& item) { return item.label == parts[0]; });
        if (top == menu.menus.end()) {
            auto window = std::find_if(menu.menus.begin(), menu.menus.end(), [](const PlatformMenuItem& item) { return item.label == "Window"; });
            top = menu.menus.insert(window, menuSubmenu(parts[0], {}));
            separated[parts[0]] = true;
        }
        insertScriptMenuItem(top->children, parts, 1, *script, separated[parts[0]]);
    }
}

editor::PlatformMenuModel editor::App::buildMenuModel(){
    SceneProject* selectedScene = project.getSelectedScene();
    uint32_t selectedSceneId = project.getSelectedSceneId();
    bool hasSelectedScene = selectedScene != nullptr;
    bool isProjectBusy = project.isAnyScenePlaying();
    const bool canCreate = hasSelectedScene && selectedScene->scene;

    // One timeline whatever window had focus (S24 R21): only a text field or the Code editor
    // keep their own undo, and then the items say plain Undo/Redo
    std::string undoLabel = "Undo";
    std::string redoLabel = "Redo";
    if (undoTarget() == UndoTimeline::Target::Timeline) {
        const uint32_t sceneId = hasSelectedScene ? selectedSceneId : NULL_PROJECT_SCENE;
        undoLabel = UndoTimeline::undoLabel(sceneId);
        redoLabel = UndoTimeline::redoLabel(sceneId);
        // the step Undo would take refused the last try (a file it changes resolves out of the
        // project through a link: the console says which and how to fix it): the menu says so
        const UndoTimeline::EntryView next = UndoTimeline::nextUndo(sceneId);
        if (next && next.history && !next.history->undoRefusedLastTry().empty()) undoLabel = "Undo (refused, see the Console)";
        // the Sprite Normal Map window's paint strokes come first
        if (lastFocusedWindow == LastFocusedWindow::SpriteNormals && spriteNormalsWindow) {
            if (spriteNormalsWindow->canUndoPaint()) undoLabel = "Undo Paint Stroke";
            if (spriteNormalsWindow->canRedoPaint()) redoLabel = "Redo Paint Stroke";
        }
    }

    // Every item below is a catalog command (shortcuts/DefaultCommands.cpp): its shortcut
    // text is the user's binding and its enabled state the command's (AppCommands.cpp).
    // Enabled items keep their native key equivalents (macOS), so single-key items are
    // disabled while typing, in the Code editor or with a game running.
    std::vector<PlatformMenuItem> recentItems;
    std::vector<std::filesystem::path> recentProjects = AppSettings::getRecentProjects();
    if (recentProjects.empty()) {
        recentItems.push_back(menuCommand(AppMenuCommand::OpenRecentProject,
                                          "No Recent Projects", false));
    } else {
        for (const auto& path : recentProjects) {
            recentItems.push_back(menuPayloadCommand(
                AppMenuCommand::OpenRecentProject,
                path.filename().string() + " (" + path.string() + ")",
                path.string()));
        }
        recentItems.push_back(menuSeparator());
        recentItems.push_back(catalogItem("file.clearRecentProjects"));
    }

    auto newSceneItems = [&]() {
        return std::vector<PlatformMenuItem>{
            catalogItem("file.newScene3D", "3D"),
            catalogItem("file.newScene2D", "2D"),
            catalogItem("file.newSceneUI", "UI")
        };
    };

    PlatformMenuModel menu;
    menu.menus.push_back(menuSubmenu("File", {
        menuSubmenu("New Scene", newSceneItems(), !isProjectBusy),
        catalogItem("file.save"),
        catalogItem("file.saveAll"),
        catalogItem("file.removeScene"),
        menuSeparator(),
        catalogItem("file.newProject"),
        catalogItem("file.openProject"),
        catalogItem("file.projectHub"),
        menuSubmenu("Recent Projects", std::move(recentItems), !isProjectBusy),
        catalogItem("file.saveProjectAs"),
        catalogItem("file.saveProjectAsTemplate"),
        menuSeparator(),
        catalogItem("file.buildSettings"),
        menuSeparator(),
        catalogItem("file.exit")
    }));

    menu.menus.push_back(menuSubmenu("Edit", {
        catalogItem("edit.undo", undoLabel),
        catalogItem("edit.redo", redoLabel),
        menuSeparator(),
        catalogItem("edit.selectAll"),
        menuSeparator(),
        catalogItem("edit.cut"),
        catalogItem("edit.copy"),
        catalogItem("edit.paste"),
        catalogItem("edit.pasteAsChild"),
        menuSeparator(),
        catalogItem("edit.duplicate"),
        catalogItem("edit.rename"),
        catalogItem("edit.delete"),
        menuSeparator(),
        catalogItem("edit.frameSelected"),
        catalogItem("edit.searchAll"),
        menuSeparator(),
        catalogItem("edit.play", "Play", mainToolbar->isInPlayMode()),
        catalogItem("edit.pause", "Pause", mainToolbar->isPaused()),
        catalogItem("edit.step"),
        catalogItem("edit.stop"),
        catalogItem("playmode.maximizeOnPlay", "Maximize On Play", gameWindow->isMaximizeOnPlay()),
        menuSeparator(),
        catalogItem("edit.projectSettings"),
        catalogItem("edit.clearUserSettings"),
        catalogItem("edit.preferences"),
        catalogItem("edit.shortcuts")
    }));

    // the one Create list (AssetCreateMenu: the Project window shows the same), then what only
    // the main menu makes: a new scene, a native plugin
    std::vector<PlatformMenuItem> assetsCreateItems;
    for (const AssetCreateItem& item : assetCreateItems()) {
        // Animation Clip: once the clip asset task set its hook
        if (item.kind == AssetCreateKind::AnimationClip && !animationClipAppHooks().createClipAsset) continue;
        if (item.separatorBefore) assetsCreateItems.push_back(menuSeparator());
        assetsCreateItems.push_back(catalogItem(item.commandId, item.label));
    }
    assetsCreateItems.push_back(menuSeparator());
    assetsCreateItems.push_back(menuSubmenu("Scene", newSceneItems(), !isProjectBusy));
    assetsCreateItems.push_back(catalogItem("assets.createNativePlugin", "Native Plugin..."));

    menu.menus.push_back(menuSubmenu("Assets", {
        menuSubmenu("Create", assetsCreateItems),
        catalogItem("assets.showInFileManager"),
        catalogItem("assets.open"),
        catalogItem("assets.rename", "Rename"),
        catalogItem("assets.duplicate", "Duplicate"),
        catalogItem("assets.delete", "Delete"),
        menuSeparator(),
        catalogItem("assets.import"),
        catalogItem("assets.refresh"),
        catalogItem("assets.reimportTextures"),
        catalogItem("assets.importSoundEssentials"),
        menuSubmenu("Version Control", {
            catalogItem("assets.vcs.commit"),
            catalogItem("assets.vcs.revert"),
            catalogItem("assets.vcs.lock"),
            catalogItem("assets.vcs.unlock"),
            menuSeparator(),
            catalogItem("assets.vcs.history"),
            catalogItem("assets.vcs.diff"),
            menuSeparator(),
            catalogItem("assets.vcs.refresh", "Refresh")
        }),
        menuSubmenu("Asset Database", {
            catalogItem("window.missingAssets", "Missing Assets..."),
            catalogItem("assets.migrateReferences")
        }),
        menuSubmenu("Scripts", {
            catalogItem("scripts.reload"),
            catalogItem("scripts.rebuildCpp"),
            catalogItem("scripts.cancelBuild")
        }),
        menuSeparator(),
        catalogItem("assets.scenesInBuild"),
        catalogItem("assets.prefabs"),
        catalogItem("assets.emptyTrash")
    }));

    // Camera Shaker on the scene's main camera (camera shake spec 9), after the create items
    std::vector<PlatformMenuItem> gameObjectItems = gameObjectMenuItems(selectedScene, canCreate);
    {
        const Entity mainCamera = hasSelectedScene ? project.getGameCamera(selectedSceneId) : NULL_ENTITY;
        const bool canAddShaker = hasSelectedScene && !isProjectBusy && mainCamera != NULL_ENTITY && selectedScene->scene &&
                                  !selectedScene->scene->findComponent<CameraShakerComponent>(mainCamera);
        gameObjectItems.push_back(menuSeparator());
        gameObjectItems.push_back(menuCommand(AppMenuCommand::AddCameraShakerToMainCamera, "Add Camera Shaker to Main Camera", canAddShaker));
    }
    menu.menus.push_back(menuSubmenu("GameObject", std::move(gameObjectItems)));

    menu.menus.push_back(menuSubmenu("Component", {
        catalogItem("component.add", "Add...")
    }));

    menu.menus.push_back(menuSubmenu("Window", {
        menuSubmenu("General", {
            catalogItem("window.scene"),
            catalogItem("window.game"),
            catalogItem("window.inspector", windowMenuLabel(InspectorWindow::WINDOW_NAME)),
            catalogItem("window.hierarchy", windowMenuLabel(HierarchyWindow::WINDOW_NAME)),
            catalogItem("window.project", windowMenuLabel(ProjectWindow::WINDOW_NAME)),
            catalogItem("window.console", windowMenuLabel(ConsoleWindow::WINDOW_NAME)),
            catalogItem("window.bookmarks", windowMenuLabel(BookmarksWindow::WINDOW_NAME)),
            catalogItem("window.undoHistory", windowMenuLabel(UndoHistoryWindow::WINDOW_NAME)),
            catalogItem("window.playModeChanges", windowMenuLabel(PlayModeChangesWindow::WINDOW_NAME)),
            catalogItem("window.colorPicker"),
            catalogItem("window.recorder", windowMenuLabel(RecorderWindow::WINDOW_NAME))
        }),
        menuSubmenu("Animation", {
            catalogItem("window.animation", windowMenuLabel(AnimationWindow::WINDOW_NAME)),
            catalogItem("window.poseAnimation", windowMenuLabel(PoseAnimationWindow::WINDOW_NAME)),
            catalogItem("window.animator", windowMenuLabel(AnimatorWindow::WINDOW_NAME)),
            catalogItem("window.vertexAnimationBaker", windowMenuLabel(VertexAnimationBakerWindow::WINDOW_NAME))
        }),
        menuSubmenu("Audio", {
            catalogItem("window.audioMixer", windowMenuLabel(AudioMixerWindow::WINDOW_NAME)),
            catalogItem("window.audioCaptures", windowMenuLabel(AudioCapturesWindow::WINDOW_NAME)),
            catalogItem("window.soundBrowser", windowMenuLabel(SoundBrowserWindow::WINDOW_NAME))
        }),
        menuSubmenu("Visual Effects", {
            catalogItem("window.particleEffect", windowMenuLabel(ParticleEffectWindow::WINDOW_NAME))
        }),
        catalogItem("window.terrain", windowMenuLabel(TerrainEditWindow::WINDOW_NAME)),
        catalogItem("window.inputActions", windowMenuLabel(InputActionsWindow::WINDOW_NAME)),
        menuSubmenu("AI", {
            catalogItem("window.navigation", windowMenuLabel(NavigationWindow::WINDOW_NAME))
        }),
        menuSubmenu("Assets", {
            catalogItem("window.missingAssets", windowMenuLabel(MissingAssetsWindow::WINDOW_NAME)),
            catalogItem("window.assetReferences", windowMenuLabel(AssetReferencesWindow::WINDOW_NAME)),
            catalogItem("window.graphEditor", windowMenuLabel(GraphEditorWindow::WINDOW_NAME))
        }),
        menuSubmenu("Analysis", {
            catalogItem("window.profiler", windowMenuLabel(ProfilerWindow::WINDOW_NAME)),
            catalogItem("window.frameDebugger", windowMenuLabel(FrameDebuggerWindow::WINDOW_NAME)),
            catalogItem("window.systems", windowMenuLabel(SystemsWindow::WINDOW_NAME)),
            catalogItem("window.jobsMemory", windowMenuLabel(JobsMemoryWindow::WINDOW_NAME)),
            catalogItem("window.meshAudit", windowMenuLabel(MeshAuditWindow::WINDOW_NAME)),
            catalogItem("window.buildReport", windowMenuLabel(BuildReportWindow::WINDOW_NAME))
        }),
        catalogItem("window.textureSynthesis", windowMenuLabel(TextureSynthesisWindow::WINDOW_NAME)),
        catalogItem("window.spriteNormals", windowMenuLabel(SpriteNormalsWindow::WINDOW_NAME)),
        catalogItem("window.physicsPlacement", windowMenuLabel(PhysicsPlacementWindow::WINDOW_NAME)),
        menuSubmenu("MCP", {
            catalogItem("window.mcpActivity", windowMenuLabel(mcp::McpActivityWindow::WINDOW_NAME))
        }),
        menuSubmenu("Version Control", {
            catalogItem("window.vcs.commit", "Commit", vcsUi->isWindowOpen(vcs::VcsUi::COMMIT_WINDOW)),
            catalogItem("window.vcs.history", "History", vcsUi->isWindowOpen(vcs::VcsUi::HISTORY_WINDOW)),
            catalogItem("window.vcs.sceneDiff", "Scene Diff", vcsUi->isWindowOpen(vcs::VcsUi::SCENE_DIFF_WINDOW)),
            catalogItem("window.vcs.conflicts", "Conflicts", vcsUi->isWindowOpen(vcs::VcsUi::CONFLICTS_WINDOW))
        }),
        menuSeparator(),
        menuSubmenu("Layouts", layoutMenuItems()),
        catalogItem("window.detachableWindows", "Detachable Windows", AppSettings::getMultiViewportEnabled())
    }));

    menu.menus.push_back(menuSubmenu("Help", {
        catalogItem("help.about"),
        menuSeparator(),
        catalogItem("help.documentation"),
        catalogItem("help.keyboardShortcuts"),
        catalogItem("help.reportIssue")
    }));
    addScriptMenus(menu);
    return menu;
}

// Window > Layouts and the toolbar's Layout menu (S25 R16): the presets (the active one
// checked), then Save Layout..., Rename Layout >, Delete Layout >, the file items, Reset Layout
std::vector<editor::PlatformMenuItem> editor::App::layoutMenuItems(){
    std::vector<PlatformMenuItem> items;
    std::vector<PlatformMenuItem> renameItems;
    std::vector<PlatformMenuItem> deleteItems;
    for (const LayoutPresetInfo& preset : layouts.presets()) {
        PlatformMenuItem apply = catalogItem("window.layouts.apply", preset.name, preset.active);
        apply.command.payload = preset.name;
        items.push_back(std::move(apply));
        if (preset.builtin) continue;
        PlatformMenuItem rename = catalogItem("window.layouts.rename", preset.name);
        rename.command.payload = preset.name;
        rename.shortcut.clear();
        renameItems.push_back(std::move(rename));
        PlatformMenuItem remove = catalogItem("window.layouts.delete", preset.name);
        remove.command.payload = preset.name;
        remove.shortcut.clear();
        deleteItems.push_back(std::move(remove));
    }
    const bool hasUserPresets = !renameItems.empty();
    items.push_back(menuSeparator());
    items.push_back(catalogItem("window.layouts.save"));
    items.push_back(menuSubmenu("Rename Layout", std::move(renameItems), hasUserPresets));
    items.push_back(menuSubmenu("Delete Layout", std::move(deleteItems), hasUserPresets));
    items.push_back(menuSeparator());
    items.push_back(catalogItem("window.layouts.saveToFile"));
    items.push_back(catalogItem("window.layouts.loadFromFile"));
    items.push_back(menuSeparator());
    items.push_back(catalogItem("window.layouts.reset"));
    return items;
}

void editor::App::drawLayoutsMenu(){
    showImGuiMenuItems(layoutMenuItems());
}

// Window menu items open the window if needed and bring it to the front (Unity).
template <typename W>
static void showEditorWindow(W* window) {
    window->setOpen(true);
    ImGui::SetWindowFocus(W::WINDOW_NAME);
}

void editor::App::executeMenuCommand(const PlatformMenuCommand& command){
    if (projectLoading || pendingProjectChange) return;

    const auto action = static_cast<AppMenuCommand>(command.id);
    // the Project Hub covers the editor: only the project commands (a native menu still shows)
    if (projectHub.isOpen() && action != AppMenuCommand::NewProject && action != AppMenuCommand::OpenProject &&
        action != AppMenuCommand::ProjectHub && action != AppMenuCommand::Exit) {
        return;
    }
    // an Edit item picked in the menu: the focused panel's claim has it (the keys, the palette and
    // agents come through the command, which asks the same)
    if (const CommandDef* def = CommandCatalog::editor().findByMenuCommand(command.id, command.payload)) {
        if (PanelClaims::editor().route(def->id)) return;
    }
    switch (action) {
        case AppMenuCommand::NewProject: {
            if (project.isAnyScenePlaying()) return;
            // the Project Hub's New Project page (S27 R17)
            auto startFreshProject = [this]() { openProjectHub(ProjectHubWindow::Page::NewProject); };
            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() ||
                project.isTempUnsavedProject()) {
                registerConfirmAlert(
                    "Unsaved Changes",
                    "There are unsaved changes. Do you want to save them before creating a new project?",
                    [this, startFreshProject]() { saveAllAndProject(startFreshProject); },
                    startFreshProject);
            } else {
                startFreshProject();
            }
            break;
        }
        case AppMenuCommand::NewScene3D:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_3D);
            break;
        case AppMenuCommand::NewScene2D:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_2D);
            break;
        case AppMenuCommand::NewSceneUI:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_UI);
            break;
        case AppMenuCommand::OpenProject:
            openProjectFunc();
            break;
        case AppMenuCommand::ProjectHub: {
            if (project.isAnyScenePlaying()) return;
            auto openHub = [this]() { openProjectHub(ProjectHubWindow::Page::Projects); };
            if (!hubOverTemp && (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges())) {
                registerConfirmAlert("Unsaved Changes", "There are unsaved changes. Do you want to save them before opening the Project Hub?",
                                     [this, openHub]() { saveAllAndProject(openHub); }, openHub);
            } else {
                openHub();
            }
            break;
        }
        case AppMenuCommand::ToggleColorPicker:
            ui::ColorPickerWindow::get().openStandalone();
            break;
        case AppMenuCommand::ToggleRecorder:
            recorderWindow.setOpen(!recorderWindow.isOpen());
            break;
        case AppMenuCommand::ToggleRecording:
            toggleRecording();
            break;
        case AppMenuCommand::SaveProjectAsTemplate:
            if (project.isTempProject()) {
                registerAlert("Save Project as Template", "Save the project first: a template is made from a saved project.");
                return;
            }
            saveTemplatePopup = true;
            std::snprintf(saveTemplateName.data(), saveTemplateName.size(), "%s", project.getName().c_str());
            saveTemplateDescription.fill(0);
            saveTemplateMessage.clear();
            break;
        case AppMenuCommand::OpenRecentProject: {
            if (command.payload.empty() || project.isAnyScenePlaying()) return;
            const std::filesystem::path path(command.payload);
            auto openRecentProject = [this, path]() {
                requestProjectChange([this, path]() { project.loadProject(path); });
            };
            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() ||
                project.isTempUnsavedProject()) {
                registerConfirmAlert(
                    "Unsaved Changes",
                    "There are unsaved changes. Do you want to save them before opening another project?",
                    [this, openRecentProject]() { saveAllAndProject(openRecentProject); },
                    openRecentProject);
            } else {
                openRecentProject();
            }
            break;
        }
        case AppMenuCommand::ClearRecentProjects:
            AppSettings::clearRecentProjects();
            break;
        case AppMenuCommand::SaveProjectAs:
            if (project.isAnyScenePlaying() || saveDialogInProgress) return;
            registerProjectSaveDialog();
            break;
        case AppMenuCommand::Save:
            saveFunc();
            break;
        case AppMenuCommand::SaveAll:
            saveAllFunc(nullptr, true);
            break;
        case AppMenuCommand::ExportProject:
            exportWindow.open(&project);
            break;
        case AppMenuCommand::Exit:
            exit();
            break;
        case AppMenuCommand::Undo:
            undoRedo(true);
            break;
        case AppMenuCommand::Redo:
            undoRedo(false);
            break;
        case AppMenuCommand::RenameSelection:
            if (!hierarchyWindow->renameSelected()) inspectorWindow->focusNameInput();
            break;
        case AppMenuCommand::SelectAll:
            selectAll();
            break;
        case AppMenuCommand::CutSelection:
            copySelection(true);
            break;
        case AppMenuCommand::CopySelection:
            copySelection(false);
            break;
        case AppMenuCommand::PasteSelection:
            pasteSelection(false);
            break;
        case AppMenuCommand::PasteAsChild:
            pasteSelection(true);
            break;
        case AppMenuCommand::FrameSelected:
            if (SceneProject* scene = project.getSelectedScene()) {
                sceneWindow->frameSelection(scene->id);
            }
            break;
        case AppMenuCommand::ToggleHierarchy:
            showEditorWindow(hierarchyWindow);
            break;
        case AppMenuCommand::ToggleInspector:
            showEditorWindow(inspectorWindow);
            break;
        case AppMenuCommand::ToggleProject:
            showEditorWindow(projectWindow);
            break;
        case AppMenuCommand::ToggleConsole:
            showEditorWindow(consoleWindow);
            break;
        case AppMenuCommand::ToggleAnimation:
            showEditorWindow(animationWindow);
            break;
        case AppMenuCommand::TogglePoseAnimation:
            showEditorWindow(poseAnimationWindow);
            break;
        case AppMenuCommand::ToggleAnimator:
            showEditorWindow(animatorWindow);
            break;
        case AppMenuCommand::ToggleTerrain:
            showEditorWindow(terrainEditWindow);
            break;
        case AppMenuCommand::ToggleProfiler:
            showEditorWindow(profilerWindow);
            break;
        case AppMenuCommand::ToggleFrameDebugger:
            showEditorWindow(frameDebuggerWindow);
            break;
        case AppMenuCommand::ToggleSystems:
            showEditorWindow(systemsWindow);
            break;
        case AppMenuCommand::ToggleJobsMemory:
            showEditorWindow(jobsMemoryWindow);
            break;
        case AppMenuCommand::ToggleMeshAudit:
            showEditorWindow(meshAuditWindow);
            break;
        case AppMenuCommand::ToggleBuildReport:
            showEditorWindow(buildReportWindow);
            break;
        case AppMenuCommand::ToggleUndoHistory:
            showEditorWindow(undoHistoryWindow);
            break;
        case AppMenuCommand::ToggleInputActions:
            showEditorWindow(inputActionsWindow);
            break;
        case AppMenuCommand::ToggleNavigation:
            showEditorWindow(navigationWindow);
            break;
        case AppMenuCommand::TogglePlayModeChanges:
            showEditorWindow(playModeChangesWindow);
            break;
        case AppMenuCommand::ToggleTextureSynthesis:
            textureSynthesisWindow->open();
            break;
        case AppMenuCommand::ToggleSoundBrowser:
            soundBrowserWindow->open();
            break;
        case AppMenuCommand::ToggleGraphEditor:
            graphEditorWindow->setOpen(true);
            graphEditorWindow->requestFocus();
            break;
        case AppMenuCommand::ToggleAudioCaptures:
            audioCapturesWindow->open();
            break;
        case AppMenuCommand::ResetLoudness:
            LoudnessMonitor::get().reset();
            break;
        case AppMenuCommand::ToggleAudioMixer:
            audioMixerWindow->open();
            break;
        case AppMenuCommand::ToggleParticleEffect:
            showEditorWindow(particleEffectWindow);
            break;
        case AppMenuCommand::ToggleSpriteNormals:
            spriteNormalsWindow->open();
            break;
        case AppMenuCommand::ToggleVertexAnimationBaker:
            vertexAnimationBakerWindow->open();
            break;
        case AppMenuCommand::TogglePhysicsPlacement:
            showEditorWindow(physicsPlacementWindow);
            break;
        case AppMenuCommand::ToggleMcpActivity:
            showEditorWindow(mcpActivityWindow);
            break;
        case AppMenuCommand::ToggleMissingAssets:
            showEditorWindow(missingAssetsWindow);
            break;
        case AppMenuCommand::ToggleAssetReferences:
            showEditorWindow(assetReferencesWindow);
            break;
        case AppMenuCommand::MigrateAssetReferences:
            migrateAssetReferences();
            break;
        case AppMenuCommand::FocusGame:
            gameWindow->requestFocus();
            break;
        case AppMenuCommand::FocusScene:
            if (project.getSelectedScene())
                ImGui::SetWindowFocus(("###Scene" + std::to_string(project.getSelectedSceneId())).c_str());
            break;
        case AppMenuCommand::ToggleDetachableWindows: {
            const bool multiViewport = !AppSettings::getMultiViewportEnabled();
            AppSettings::setMultiViewportEnabled(multiViewport);
            AppSettings::saveSettings();
            if (multiViewport && !Backend::isRunningOnWayland())
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
            else
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
#ifdef __linux__
            if (multiViewport) {
                if (Backend::isRunningOnWayland())
                    registerAlert("Restart Required",
                        "Detachable windows will take effect after you restart the editor.",
                        "Note: This feature may encounter issues on Linux.");
                else
                    registerAlert("Experimental Feature",
                        "Detachable windows are now enabled.",
                        "Note: This feature may encounter issues on Linux.");
            }
#endif
            break;
        }
        case AppMenuCommand::ResetLayout:
            // Unity's Reset Layout: the Default preset
            layouts.requestSwitch(LayoutPresets::DEFAULT_NAME);
            break;
        case AppMenuCommand::ProjectSettings:
            projectSettingsWindow.open(&project);
            break;
        case AppMenuCommand::EditorSettings:
            editorSettingsWindow.open(&project);
            break;
        case AppMenuCommand::ClearUserSettings: {
            // the store Play mode uses (.jasperengine/user/rundata/UserSettings.xml): a running
            // game sees the keys gone at once
            const size_t count = UserSettingsStore::instance().entries().size();
            const std::string file = UserSettingsStore::instance().getFilePath();
            registerConfirmAlert("Clear User Settings",
                "Delete every UserSettings / PlayerPrefs key of this project (" + std::to_string(count) +
                (count == 1 ? " key" : " keys") + ")?\n" + file + "\nThis cannot be undone.",
                []() {
                    UserSettings::deleteAll();
                    UserSettings::save();
                    Out::info("Cleared the project's User Settings (PlayerPrefs)");
                });
            break;
        }
        case AppMenuCommand::ProjectScenes:
            if (!project.isAnyScenePlaying())
                scenesWindow.open(&project);
            break;
        case AppMenuCommand::ProjectPrefabs:
            prefabsWindow.open(&project);
            break;
        case AppMenuCommand::ClearTrash: {
            if (project.isAnyScenePlaying()) return;
            const auto projectPath = project.getProjectPath();
            registerConfirmAlert("Empty Trash",
                "Permanently delete all files in this project's trash?\nThis cannot be undone.",
                [this, projectPath]() {
                    if (project.getProjectPath() == projectPath && !project.isAnyScenePlaying()) project.clearTrash();
                });
            break;
        }
        case AppMenuCommand::TogglePlay:
            // Unity's Play toggles: starts the scene, or stops it when running
            mainToolbar->togglePlay();
            break;
        case AppMenuCommand::TogglePause:
            mainToolbar->togglePause();
            break;
        case AppMenuCommand::StepFrame:
            mainToolbar->step();
            break;
        case AppMenuCommand::StopScene:
            if (mainToolbar->isInPlayMode()) mainToolbar->togglePlay();
            break;
        case AppMenuCommand::RemoveScene: {
            const uint32_t sceneId = project.getSelectedSceneId();
            if (project.getSelectedScene() && !project.isAnyScenePlaying() &&
                project.getScenes().size() > 1) {
                project.checkUnsavedAndExecute(sceneId, [this, sceneId]() {
                    project.removeScene(sceneId);
                });
            }
            break;
        }
        case AppMenuCommand::CreateGameObject: {
            const auto& entries = HierarchyWindow::getCreateMenuEntries();
            const size_t index = command.payload.empty() ? entries.size()
                : static_cast<size_t>(std::strtoul(command.payload.c_str(), nullptr, 10));
            SceneProject* scene = project.getSelectedScene();
            if (index < entries.size() && scene && scene->scene &&
                HierarchyWindow::isCreateEntryAvailable(entries[index], scene->sceneType)) {
                hierarchyWindow->createGameObject(entries[index], NULL_ENTITY);
            }
            break;
        }
        case AppMenuCommand::CreateEmptyChild: {
            const Entity parent = hierarchyWindow->getSelectedParentForCreate();
            if (parent == NULL_ENTITY) break;
            for (const CreateMenuEntry& entry : HierarchyWindow::getCreateMenuEntries()) {
                if (entry.type == EntityCreationType::GAMEOBJECT) {
                    hierarchyWindow->createGameObject(entry, parent);
                    break;
                }
            }
            break;
        }
        case AppMenuCommand::AddComponent: {
            SceneProject* scene = project.getSelectedScene();
            if (scene && !project.isAnyScenePlaying() && !project.getSelectedEntities(scene->id).empty())
                inspectorWindow->requestAddComponent();
            break;
        }
        // Assets > Create: in the folder the Project window shows (the one Create list, AssetCreateMenu)
        case AppMenuCommand::AssetsCreateFolder:
            projectWindow->createAsset(AssetCreateKind::Folder, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateLuaScript:
            projectWindow->createAsset(AssetCreateKind::LuaScript, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateCppScript:
            projectWindow->createAsset(AssetCreateKind::CppScript, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateCppSource:
            projectWindow->createAsset(AssetCreateKind::CppSource, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateCppHeader:
            projectWindow->createAsset(AssetCreateKind::CppHeader, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateHtmlDocument:
            projectWindow->createAsset(AssetCreateKind::HtmlDocument, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsImportSoundEssentials:
            projectWindow->assetsImportSoundEssentials();
            break;
        case AppMenuCommand::AssetsCreatePlaceholderSound:
            projectWindow->createAsset(AssetCreateKind::PlaceholderSound, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsReanalyzeAudio:
            projectWindow->assetsReanalyzeAudio(projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::AssetsCreateAudioMixer:
            projectWindow->createAsset(AssetCreateKind::AudioMixer, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateSoundEvent:
            projectWindow->createAsset(AssetCreateKind::SoundEvent, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateSoundEventFromClips:
            projectWindow->assetsCreateSoundEventFromClips();
            break;
        case AppMenuCommand::AssetsCreatePoseAnimation:
            projectWindow->createAsset(AssetCreateKind::PoseAnimation, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateShakePreset:
            projectWindow->createAsset(AssetCreateKind::ShakePreset, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreatePhysicsMaterial:
            projectWindow->createAsset(AssetCreateKind::PhysicsMaterial, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreatePhysicsMaterial2D:
            projectWindow->createAsset(AssetCreateKind::PhysicsMaterial2D, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsDuplicate:
            projectWindow->duplicateSelection();
            break;
        case AppMenuCommand::ToggleMaximizeOnPlay:
            gameWindow->toggleMaximizeOnPlay();
            break;
        case AppMenuCommand::AddCameraShakerToMainCamera: {
            SceneProject* scene = project.getSelectedScene();
            const Entity mainCamera = scene ? project.getGameCamera(scene->id) : NULL_ENTITY;
            if (scene && scene->scene && !project.isAnyScenePlaying() && mainCamera != NULL_ENTITY &&
                !scene->scene->findComponent<CameraShakerComponent>(mainCamera)) {
                CommandHandle::get(scene->id)->addCommandNoMerge(
                    new AddComponentCmd(&project, scene->id, mainCamera, ComponentType::CameraShakerComponent));
                project.setSelectedEntity(scene->id, mainCamera);
            }
            break;
        }
        case AppMenuCommand::AssetsCreateNativePlugin:
            nativePluginDialog.open(&project);
            break;
        case AppMenuCommand::AssetsCreateAnimationClip:
            if (animationClipAppHooks().createClipAsset) {
                animationClipAppHooks().createClipAsset(&project);
            }
            break;
        case AppMenuCommand::AssetsCreateAnimatorController:
            projectWindow->createAsset(AssetCreateKind::AnimatorController, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateAvatarMask:
            projectWindow->createAsset(AssetCreateKind::AvatarMask, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsCreateAnimationMontage:
            projectWindow->createAsset(AssetCreateKind::AnimationMontage, projectWindow->createTargetDirectory());
            break;
        case AppMenuCommand::AssetsShowInFileManager:
            projectWindow->assetsShowInFileManager();
            break;
        case AppMenuCommand::AssetsOpen:
            projectWindow->assetsOpenSelected();
            break;
        case AppMenuCommand::AssetsRename:
            projectWindow->assetsRenameSelected();
            break;
        case AppMenuCommand::AssetsDelete:
            projectWindow->assetsDeleteSelected();
            break;
        case AppMenuCommand::AssetsImport:
            projectWindow->assetsImport();
            break;
        case AppMenuCommand::VcsCommit:
            vcsUi->run(vcs::VcsAction::Commit, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsRevert:
            vcsUi->run(vcs::VcsAction::Revert, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsLock:
            vcsUi->run(vcs::VcsAction::Lock, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsUnlock:
            vcsUi->run(vcs::VcsAction::Unlock, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsShowHistory:
            vcsUi->run(vcs::VcsAction::ShowHistory, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsDiffAgainstBaseline:
            vcsUi->run(vcs::VcsAction::DiffAgainstBaseline, projectWindow->getSelectedFiles());
            break;
        case AppMenuCommand::VcsRefresh:
            vcsUi->run(vcs::VcsAction::Refresh, {});
            break;
        case AppMenuCommand::ToggleVcsCommit:
            vcsUi->toggleWindow(vcs::VcsUi::COMMIT_WINDOW);
            break;
        case AppMenuCommand::ToggleVcsHistory:
            vcsUi->toggleWindow(vcs::VcsUi::HISTORY_WINDOW);
            break;
        case AppMenuCommand::ToggleVcsSceneDiff:
            vcsUi->toggleWindow(vcs::VcsUi::SCENE_DIFF_WINDOW);
            break;
        case AppMenuCommand::ToggleVcsConflicts:
            vcsUi->toggleWindow(vcs::VcsUi::CONFLICTS_WINDOW);
            break;
        case AppMenuCommand::AssetsRefresh:
            // what changed on disk (the journal with Auto Refresh off), then the open folder
            HotReloadService::get().refresh();
            projectWindow->assetsRefresh();
            break;
        case AppMenuCommand::AssetsReimportTextures:
            if (TextureCookQueue::get().getProject() == &project) {
                TextureCookQueue::get().invalidateAll();
                TextureCookQueue::get().requestAll({TextureCookQueue::get().getViewFamily()});
            }
            break;
        case AppMenuCommand::About:
            aboutWindow.open();
            break;
        case AppMenuCommand::ScriptsReload:
            ScriptReloadService::get().requestReload({}, true, ScriptReloadService::cppReloadEnabled());
            break;
        case AppMenuCommand::ScriptsRebuildCpp: {
            std::string message;
            if (!ScriptReloadService::get().rebuildCpp(message)) Out::warning("Rebuild C++ Scripts: %s", message.c_str());
            break;
        }
        case AppMenuCommand::ScriptsCancelBuild:
            ScriptReloadService::get().cancelCppBuild();
            break;
        case AppMenuCommand::ScriptMenu: {
            EditorMenuRunContext context;
            context.sceneId = project.getSelectedSceneId();
            for (Entity entity : project.getSelectedEntities(context.sceneId)) context.selection.push_back(entity);
            if (projectWindow) {
                for (const fs::path& path : projectWindow->getSelectedFiles()) {
                    const fs::path rel = path.lexically_relative(project.getProjectPath());
                    context.assets.push_back(rel.generic_string());
                }
            }
            EditorScripts::get().runFromMenu(command.payload, context);
            break;
        }
        case AppMenuCommand::DuplicateSelection:
            duplicateSelection();
            break;
        case AppMenuCommand::DeleteSelection:
            // the Draw Spline tool's last knot, the edited spline's knots
            if (!SplineSceneTools::deleteRequested(&project)) deleteSelection();
            break;
        case AppMenuCommand::Documentation:
        case AppMenuCommand::ReportIssue: {
            const char* url = action == AppMenuCommand::Documentation
                ? "https://github.com/JasperRaccoon/JasperEngine" : "https://github.com/JasperRaccoon/JasperEngine/issues";
            auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
            if (!openInShell || !openInShell(ImGui::GetCurrentContext(), url)) {
                registerAlert("Open in Browser", std::string("Could not open your browser. Visit:\n") + url);
            }
            break;
        }
        case AppMenuCommand::KeyboardShortcuts:
            // generated from the command catalog, filterable (R5)
            shortcutsWindow->openReference();
            break;
        case AppMenuCommand::Shortcuts:
            shortcutsWindow->openEditor();
            break;
        case AppMenuCommand::SearchAll:
            searchPalette->open();
            break;
        case AppMenuCommand::ToggleBookmarks:
            showEditorWindow(bookmarksWindow);
            break;
        case AppMenuCommand::LayoutApply:
            layouts.requestSwitch(command.payload);
            break;
        case AppMenuCommand::LayoutSave:
            layouts.openSaveDialog();
            break;
        case AppMenuCommand::LayoutRename:
            layouts.openRenameDialog(command.payload);
            break;
        case AppMenuCommand::LayoutDelete:
            layouts.confirmDelete(command.payload);
            break;
        case AppMenuCommand::LayoutSaveToFile:
            layouts.exportToFile();
            break;
        case AppMenuCommand::LayoutLoadFromFile:
            layouts.importFromFile();
            break;
    }
}

void editor::App::showImGuiMenuItems(const std::vector<PlatformMenuItem>& items){
    for (const PlatformMenuItem& item : items) {
        if (item.type == PlatformMenuItemType::Separator) {
            ImGui::Separator();
        } else if (item.type == PlatformMenuItemType::Submenu) {
            if (ImGui::BeginMenu(item.label.c_str(), item.enabled)) {
                // editor scripts' ---@validate run when a menu with their items opens
                if (ImGui::IsWindowAppearing()) validateScriptMenus(item);
                showImGuiMenuItems(item.children);
                ImGui::EndMenu();
            }
        } else {
            if (ImGui::MenuItem(item.label.c_str(), item.shortcut.empty() ? nullptr : item.shortcut.c_str(), item.checked, item.enabled)) {
                executeMenuCommand(item.command);
            }
            if (!item.enabled && !item.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", item.tooltip.c_str());
            }
        }
    }
}

void editor::App::validateScriptMenus(const PlatformMenuItem& menu){
    bool hasScripts = false;
    std::function<void(const PlatformMenuItem&)> scan = [&](const PlatformMenuItem& item) {
        if (item.command.id == static_cast<uint32_t>(AppMenuCommand::ScriptMenu)) hasScripts = true;
        for (const PlatformMenuItem& child : item.children) scan(child);
    };
    scan(menu);
    if (!hasScripts) return;
    EditorMenuRunContext context;
    context.sceneId = project.getSelectedSceneId();
    for (Entity entity : project.getSelectedEntities(context.sceneId)) context.selection.push_back(entity);
    std::function<void(const PlatformMenuItem&)> validate = [&](const PlatformMenuItem& item) {
        if (item.command.id == static_cast<uint32_t>(AppMenuCommand::ScriptMenu)) {
            if (const EditorMenuItem* script = EditorScripts::get().find(item.command.payload)) EditorScripts::get().menuOpened(script->path, context);
        }
        for (const PlatformMenuItem& child : item.children) validate(child);
    };
    validate(menu);
}

void editor::App::showImGuiMenu(const PlatformMenuModel& menu){
    if (ImGui::BeginMainMenuBar()) {
        for (const PlatformMenuItem& topLevel : menu.menus) {
            if (ImGui::BeginMenu(topLevel.label.c_str(), topLevel.enabled)) {
                // Paste reflects what another editor instance or application copied
                if (topLevel.label == "Edit" && ImGui::IsWindowAppearing()) {
                    EntityClipboard::refresh();
                }
                if (ImGui::IsWindowAppearing()) validateScriptMenus(topLevel);
                showImGuiMenuItems(topLevel.children);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMainMenuBar();
    }
}

editor::UndoTimeline::Target editor::App::undoTarget() const{
    UndoTimeline::FocusedWindow focused = UndoTimeline::FocusedWindow::None;
    switch (lastFocusedWindow) {
        case LastFocusedWindow::None: focused = UndoTimeline::FocusedWindow::None; break;
        case LastFocusedWindow::AnySceneWindow: focused = UndoTimeline::FocusedWindow::Scene; break;
        case LastFocusedWindow::Resources: focused = UndoTimeline::FocusedWindow::Resources; break;
        case LastFocusedWindow::Code: focused = UndoTimeline::FocusedWindow::Code; break;
        // their edits are steps of the project history: the timeline orders them with the scene's
        case LastFocusedWindow::PoseAnimation:
        case LastFocusedWindow::AnimationClip:
        // its assigns are steps too; its paint strokes are its own (undoRedo takes them first)
        case LastFocusedWindow::SpriteNormals: focused = UndoTimeline::FocusedWindow::Other; break;
    }
    return UndoTimeline::resolveTarget(ImGui::GetIO().WantTextInput, codeEditor->isFocused(), focused);
}

void editor::App::undoRedo(bool undo){
    switch (undoTarget()) {
        case UndoTimeline::Target::TextField:
            return;
        case UndoTimeline::Target::CodeEditor:
            if (undo) codeEditor->undoLastFocused();
            else codeEditor->redoLastFocused();
            return;
        case UndoTimeline::Target::Timeline:
            break;
    }
    // the Sprite Normal Map window: its paint strokes (window-local until Save) first, then the
    // scene of its last Save & Assign while that assign is the scene's newest step
    if (lastFocusedWindow == LastFocusedWindow::SpriteNormals && spriteNormalsWindow) {
        if (undo ? spriteNormalsWindow->canUndoPaint() : spriteNormalsWindow->canRedoPaint()) {
            if (undo) spriteNormalsWindow->undoPaint();
            else spriteNormalsWindow->redoPaint();
            return;
        }
        // another history's step undone: the stroke redos go (Ctrl+Y redoes that step first)
        if (undo) spriteNormalsWindow->dropPaintRedo();
        uint32_t routedScene = 0;
        if (undo ? spriteNormalsWindow->undoGoesToScene(routedScene) : spriteNormalsWindow->redoGoesToScene(routedScene)) {
            if (undo) CommandHandle::get(routedScene)->undo();
            else CommandHandle::get(routedScene)->redo();
            return;
        }
    }
    const bool dragging = CommandHistory::anyInteractiveOpen();
    std::string why;
    const uint32_t sceneId = project.getSelectedScene() ? project.getSelectedSceneId() : NULL_PROJECT_SCENE;
    const bool done = undo ? UndoTimeline::undo(sceneId, &why) : UndoTimeline::redo(sceneId, &why);
    if (dragging) {
        // the cancelled drag's widget lets go of the mouse
        ImGui::ClearActiveID();
    }
    if (!done && why.rfind("Busy", 0) == 0) {
        Out::warning(why);
    }
}

void editor::App::handleInteractiveEdits(bool popupOpen){
    ImGuiIO& io = ImGui::GetIO();
    // Esc puts a drag's values back; a text field keeps Esc for its own revert. First: ImGui
    // lets go of the dragged widget on Esc itself, which must not read as a drag that ended
    if (CommandHistory::anyInteractiveOpen() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        for (CommandHistory* history : std::vector<CommandHistory*>(CommandHistory::all())) {
            history->cancelInteractive();
        }
        ImGui::ClearActiveID();
        return;
    }
    // the widget let go last frame: its drag is one step
    const ImGuiID active = ImGui::GetActiveID();
    if (widgetEdit.isOpen() && active != widgetEditItem) {
        widgetEdit.commit();
    }
    if (active == 0) {
        // no widget can hold a command the history dropped
        CommandHistory::flushReleased();
    }
    if (!CommandHistory::anyInteractiveOpen()) {
        return;
    }
    // A menu or popup opening, or the editor window losing focus, ends the drag as it is
    if (popupOpen || io.AppFocusLost) {
        CommandHistory::commitInteractiveEdits();
    }
}

// The catalog learns where each command sits in the main menu (the palette's detail, the
// Shortcuts window's menu path) and which chord its item shows (macOS NSMenu owns that one)
void editor::App::recordMenuItems(const PlatformMenuModel& menu){
    CommandCatalog& catalog = CommandCatalog::editor();
    catalog.clearMenuItems();
    std::function<void(const std::vector<PlatformMenuItem>&, const std::string&)> walk =
        [&](const std::vector<PlatformMenuItem>& items, const std::string& path) {
            for (const PlatformMenuItem& item : items) {
                if (item.type == PlatformMenuItemType::Separator) continue;
                const std::string itemPath = path.empty() ? item.label : path + " > " + item.label;
                if (item.type == PlatformMenuItemType::Submenu) {
                    walk(item.children, itemPath);
                    continue;
                }
                const CommandDef* def = catalog.findByMenuCommand(item.command.id, item.command.payload);
                if (!def || !catalog.menuPath(def->id).empty()) continue;
                catalog.setMenuItem(def->id, itemPath,
                                    item.shortcut.empty() ? Chord() : catalog.bindings(PanelClaims::editor().shownCommand(def->id)).front());
            }
        };
    walk(menu.menus, "");
}

void editor::App::showMenu(){
    PlatformMenuModel menu = buildMenuModel();
    if (menu.menus != lastMenuModel.menus) {
        recordMenuItems(menu);
        lastMenuModel = menu;
    }
    const float nativeMenuHeight = Backend::setMainMenu(
        menu, [this](const PlatformMenuCommand& command) {
            executeMenuCommand(command);
        });
    if (nativeMenuHeight < 0.0f)
        return;
    if (nativeMenuHeight > 0.0f) {
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav;
        ImGui::BeginViewportSideBar("##NativeMainMenuSpace", ImGui::GetMainViewport(),
                                    ImGuiDir_Up, nativeMenuHeight, flags);
        ImGui::End();
        return;
    }
    showImGuiMenu(menu);
}

void editor::App::showFooter(){
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    // Unity greys the whole editor while in Play mode. Set here, before the
    // dockspace and panels draw, so the tint lands in the same frame.
    Theme::setPlayModeTint(project.isPlaySessionActive());

    const float fontScale = 0.9f;
    const float footerHeight = std::floor((ImGui::GetTextLineHeight() * fontScale) + Theme::dpi(8.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoFocusOnAppearing;

    // Position at the bottom of the main viewport
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - footerHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, footerHeight));
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(6.0f, 4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Theme::dpi(ImVec2(6.0f, 0.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

    if (ImGui::Begin("##Footer", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 winSize = ImGui::GetWindowSize();
        // Separator line along the top, like Unity's status bar.
        drawList->AddLine(winPos, ImVec2(winPos.x + winSize.x, winPos.y),
                          ImGui::GetColorU32(ImGuiCol_Border), Theme::dpi(1.0f));

        const float fps = footerFramerate;
        const float deltaMs = footerDeltaMs;
        const size_t queuedResources = Engine::getQueuedResourceCount();
        SceneProject* selectedScene = project.getSelectedScene();
        const bool hasSelectedScene = selectedScene != nullptr;
        const bool isSaving = hasSelectedScene && selectedScene->playState == ScenePlayState::SAVING;
        const bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;
        const bool isCancelling = hasSelectedScene && selectedScene->playState == ScenePlayState::CANCELLING;
        const bool isAnySaving = project.isAnySceneSaving();

        // Right side, built first so the console message knows how much room it has.
        std::vector<std::pair<std::string, ImVec4>> indicators;
        const ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const ImVec4 dimCol = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const ImVec4 busyCol = ImVec4(0.48f, 0.68f, 0.98f, 1.0f);
        if (isSaving || isAnySaving) {
            indicators.emplace_back(ICON_FA_FLOPPY_DISK " Saving scene", busyCol);
        } else if (isLoading) {
            indicators.emplace_back(ICON_FA_SPINNER " Loading scene", busyCol);
        } else if (isCancelling) {
            indicators.emplace_back(ICON_FA_STOP " Stopping", Theme::Colors::WarningText);
        }
        if (queuedResources > 0) {
            indicators.emplace_back(std::string(ICON_FA_SPINNER " Loading resources: ") + std::to_string(queuedResources), busyCol);
        }
        // S04 R27: the Missing Assets badge, a click opens the panel
        size_t missingIndicator = SIZE_MAX;
        if (project.getAssetIndex().isReady()) {
            const size_t missing = project.getMissingAssets().count();
            if (missing > 0) {
                missingIndicator = indicators.size();
                indicators.emplace_back(std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(missing) +
                                        (missing == 1 ? " missing asset" : " missing assets"), Theme::Colors::WarningText);
            }
        }
        if (selectedScene) {
            indicators.emplace_back(std::string(ICON_FA_CUBE " ") + selectedScene->name, dimCol);
            const size_t selectionCount = selectedScene->selectedEntities.size();
            if (selectionCount == 1) {
                indicators.emplace_back("1 GameObject selected", dimCol);
            } else if (selectionCount > 1) {
                indicators.emplace_back(std::to_string(selectionCount) + " GameObjects selected", dimCol);
            }
        }
        char statText[160];
        const Engine::FrameStats& frameStats = Engine::getFrameStats();
        snprintf(statText, sizeof(statText), ICON_FA_GAUGE_HIGH " %.1f FPS  %.2f ms", fps, deltaMs);
        const size_t fpsIndicator = indicators.size();
        indicators.emplace_back(statText, textCol);
        snprintf(statText, sizeof(statText), ICON_FA_CUBES " %u  " ICON_FA_DRAW_POLYGON " %llu",
                 frameStats.drawCalls, (unsigned long long)frameStats.triangles);
        indicators.emplace_back(statText, textCol);

        const float gap = Theme::dpi(14.0f);
        float indicatorsWidth = 0.0f;
        for (const auto& indicator : indicators) {
            indicatorsWidth += ImGui::CalcTextSize(indicator.first.c_str()).x + gap;
        }
        const float rightStart = ImGui::GetWindowContentRegionMax().x - indicatorsWidth + gap;

        // Left side: the newest Console message; a click brings up the Console.
        LogType latestType = LogType::Info;
        std::string latestMessage;
        if (consoleWindow && consoleWindow->getLatestLog(latestType, latestMessage)) {
            const size_t lineEnd = latestMessage.find_first_of("\r\n");
            if (lineEnd != std::string::npos) latestMessage.resize(lineEnd);

            const char* icon = ICON_FA_CIRCLE_INFO;
            ImVec4 iconCol = Theme::Colors::LogIcon;
            if (latestType == LogType::Warning) {
                icon = ICON_FA_TRIANGLE_EXCLAMATION;
                iconCol = Theme::Colors::WarningText;
            } else if (latestType == LogType::Error) {
                icon = ICON_FA_CIRCLE_EXCLAMATION;
                iconCol = Theme::Colors::ErrorText;
            } else if (latestType == LogType::Success) {
                icon = ICON_FA_CIRCLE_CHECK;
                iconCol = ImVec4(0.45f, 0.80f, 0.45f, 1.0f);
            } else if (latestType == LogType::Build) {
                icon = ICON_FA_HAMMER;
                iconCol = ImVec4(0.55f, 0.72f, 0.90f, 1.0f);
            }

            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float maxWidth = std::max(0.0f, rightStart - ImGui::GetCursorPosX() - gap);
            ImGui::InvisibleButton("##FooterConsole", ImVec2(maxWidth, ImGui::GetTextLineHeight()));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                consoleWindow->requestFocus();
            }
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open the Console");
            }
            drawList->AddText(start, ImGui::GetColorU32(iconCol), icon);
            const float textX = start.x + ImGui::CalcTextSize(icon).x + Theme::dpi(6.0f);
            const ImVec4 clip(textX, start.y, start.x + maxWidth, start.y + ImGui::GetTextLineHeight());
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textX, start.y),
                              ImGui::GetColorU32(hovered ? textCol : ImVec4(textCol.x, textCol.y, textCol.z, 0.85f)),
                              latestMessage.c_str(), nullptr, 0.0f, &clip);
        }

        // Right side: status indicators and render stats.
        if (rightStart > ImGui::GetCursorPosX()) {
            ImGui::SameLine(rightStart);
        } else {
            ImGui::SameLine();
        }
        for (size_t i = 0; i < indicators.size(); i++) {
            if (i > 0) ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(indicators[i].second, "%s", indicators[i].first.c_str());
            if (i == missingIndicator) {
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Open Window > Assets > Missing Assets");
                }
                if (ImGui::IsItemClicked()) {
                    showEditorWindow(missingAssetsWindow);
                }
            }
            if (i == fpsIndicator) {
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Open Window > Analysis > Profiler");
                }
                if (ImGui::IsItemClicked()) {
                    showEditorWindow(profilerWindow);
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    // Reserve space from the viewport work area so dockspace doesn't overlap
    viewport->WorkSize.y -= footerHeight;
}

void editor::App::showCrashNotice(){
    // after the S03 "Recover Unsaved Changes" modal and the other data-safety questions
    if (dataSafetyDialogs.isShowing()) return;
    const std::optional<diag::crash::ReportInfo> notice = EditorDiagnostics::notice();
    if (!notice) return;
    const std::string text = EditorDiagnostics::noticeText(*notice);
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.36f, 0.27f, 0.08f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(8.0f, 4.0f)));
    if (ImGui::BeginViewportSideBar("##CrashNotice", viewport, ImGuiDir_Up, ImGui::GetFrameHeight() + Theme::dpi(8.0f), flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextUnformatted(text.c_str());
        const ImGuiStyle& style = ImGui::GetStyle();
        const char* labels[] = {"Open Report", "Show in Folder", "Copy Report Path", "Dismiss"};
        float buttons = 0.0f;
        for (const char* label : labels) buttons += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttons + style.ItemSpacing.x));
        if (ImGui::Button("Open Report")) {
            EditorSettingsWindow::openInFileManager(EditorDiagnostics::primaryFile(*notice));
        }
        ImGui::SameLine();
        if (ImGui::Button("Show in Folder")) {
            EditorSettingsWindow::openInFileManager(notice->folder);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy Report Path")) {
            ImGui::SetClipboardText(notice->folder.string().c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) {
            EditorDiagnostics::dismiss(notice->id);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void editor::App::showHangToast(){
    const std::optional<EditorDiagnostics::Toast> toast = EditorDiagnostics::activeToast();
    if (!toast) return;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // the work area ends at the footer's top (showFooter reserved it)
    const ImVec2 corner(viewport->WorkPos.x + viewport->WorkSize.x - Theme::dpi(12.0f),
                        viewport->WorkPos.y + viewport->WorkSize.y - Theme::dpi(12.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(corner, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##HangToast", nullptr, flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextUnformatted(toast->text.c_str());
        if (!toast->file.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                EditorSettingsWindow::openInFileManager(toast->file);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##CloseHangToast")) {
            EditorDiagnostics::closeToast();
        }
    }
    ImGui::End();
}

// Assets > Asset Database > Migrate References (S04 R35): every scene, prefab, material and
// project.yaml written with the GUIDs of what it references. A document with unsaved edits is
// left to its next save, which writes them too.
void editor::App::migrateAssetReferences(){
    project.getAssetIndex().waitUntilReady();
    std::vector<std::string> files;
    project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type == AssetType::Scene || info.type == AssetType::Prefab || info.type == AssetType::Material) {
            files.push_back(info.path);
        }
    });
    std::sort(files.begin(), files.end());
    files.push_back("project.yaml");
    size_t written = 0;
    size_t skipped = 0;
    for (const std::string& file : files) {
        bool unsaved = false;
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            const PrefabStage* stage = project.getPrefabStage(sceneProject->id);
            const fs::path path = stage ? stage->assetPath : sceneProject->filepath;
            unsaved |= sceneProject->scene && sceneProject->isModified && !path.empty() &&
                       project.normalizeToProjectRelative(path).generic_string() == file;
        }
        if (unsaved) {
            skipped++;
            continue;
        }
        bool changed = false;
        std::string error;
        if (!AssetRefEditor::migrateFile(project, file, changed, error)) {
            Out::error("%s", error.c_str());
        } else if (changed) {
            written++;
        }
    }
    project.refreshAssetDatabase(true);
    Out::info("Migrate References: %zu file(s) written%s", written,
              skipped ? (", " + std::to_string(skipped) + " with unsaved edits get the GUIDs when saved").c_str() : "");
}

void editor::App::showAlert(){
    if (alert.needShow && !alertPendingChoice.empty()) {
        // An MCP agent answered the alert (respond_dialog): press that button
        const std::string choice = std::move(alertPendingChoice);
        alertPendingChoice.clear();
        const std::string popupName = alert.title + "##AlertModal";
        alert.needShow = false;
        if (ImGui::IsPopupOpen(popupName.c_str()) && EditorModal::begin(popupName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        std::function<void()> callback;
        if (choice == "yes") callback = alert.onYes;
        else if (choice == "no") callback = alert.onNo;
        else if (choice == "cancel") callback = alert.onCancel;
        if (callback) callback();
        return;
    }
    if (alert.needShow) {
        ImGui::OpenPopup((alert.title + "##AlertModal").c_str());

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_Modal;

        if (EditorModal::begin((alert.title + "##AlertModal").c_str(), nullptr, flags)) {
            ImGui::Text("%s", alert.message.c_str());
            if (!alert.note.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Theme::Colors::WarningText, "%s", alert.note.c_str());
            }
            ImGui::Separator();

            if (alert.type == AlertType::Info) {
                // For info alerts, just show OK button
                ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                }
            } else if (alert.type == AlertType::Confirm) {
                // For confirmation alerts, show Yes and No buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 250; // Total width for both buttons and spacing
                ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

                if (ImGui::Button("Yes", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        alert.onYes();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        alert.onNo();
                    }
                }
            } else if (alert.type == AlertType::ThreeButton) {
                // For three-button alerts, show Yes, No and Cancel buttons (or their labels)
                float windowWidth = ImGui::GetWindowSize().x;
                const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
                const float yesWidth = std::max(120.0f, ImGui::CalcTextSize(alert.yesLabel.c_str()).x + padding);
                const float noWidth = std::max(120.0f, ImGui::CalcTextSize(alert.noLabel.c_str()).x + padding);
                float buttonsWidth = yesWidth + noWidth + 120.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
                ImGui::SetCursorPosX(std::max(0.0f, (windowWidth - buttonsWidth) * 0.5f));

                if (ImGui::Button(alert.yesLabel.c_str(), ImVec2(yesWidth, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        alert.onYes();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(alert.noLabel.c_str(), ImVec2(noWidth, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        alert.onNo();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onCancel) {
                        alert.onCancel();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}

void editor::App::buildDockspace(bool resetLayout){
    // A reset forces every tab back to its default slot even if the ini has a
    // customized position saved for it (see dockTabWindow()).
    forceDockTabs = resetLayout;

    // Keep an existing layout (restored from the persisted ini) so the user's
    // customizations survive; only recover the scene node we dock tabs into.
    // A reset rebuilds the default skeleton from scratch instead.
    if (!resetLayout && ImGui::DockBuilderGetNode(dockspace_id) != nullptr) {
        dock_id_middle_top = getCentralDockId();
    } else {
        buildDefaultLayout();
        cappedDockSizes.clear();
        // Built from the current font size, so it needs no DPI correction.
        layoutUiScale = mainViewportScale();
        layoutScaleApplied = true;
    }

    dockProjectTabs();

    ImGui::DockBuilderFinish(dockspace_id);

    forceDockTabs = false;
}

// The ini stores dock sizes in absolute pixels and ImGui never converts them, so
// a layout saved at 100% stays narrow at 150% on every launch.
void editor::App::rescaleRestoredLayout(){
    if (layoutScaleApplied) return;

    // Nothing persisted: buildDefaultLayout() builds at the current scale below.
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
    if (!root) return;

    const float currentScale = mainViewportScale();
    const float savedScale = sanitizeLayoutScale(layoutUiScale);
    layoutUiScale = currentScale;
    layoutScaleApplied = true;

    const float ratio = currentScale / savedScale;
    if (ratio > 0.99f && ratio < 1.01f) return;

    // The root node is pinned to the viewport, so only its descendants move.
    scaleDockChildren(root, ratio);
}

// SizeRef against a central node is read as an absolute size, so it has to grow
// with the text; siblings split by ratio stay proportional under the same factor.
void editor::App::scaleDockChildren(ImGuiDockNode* node, float ratio){
    const ImVec2 available = ImGui::GetMainViewport()->WorkSize;
    for (int i = 0; i < 2; i++) {
        ImGuiDockNode* child = node->ChildNodes[i];
        if (!child) continue;
        const ImVec2 sizeRef = child->SizeRef;
        child->Size = ImVec2(child->Size.x * ratio, child->Size.y * ratio);
        child->SizeRef = ImVec2(sizeRef.x * ratio, sizeRef.y * ratio);

        // Panels beside the scene only grow up to the cap, or the scene gets
        // squeezed out. One already past its cap keeps its size.
        if (holdsCentralNode(node->ChildNodes[i ^ 1]) && !holdsCentralNode(child)) {
            const int axis = node->SplitAxis;
            const float cap = available[axis] * panelMaxShare;
            float uncapped = sizeRef[axis] * ratio;
            // Resume from the uncapped size unless the panel was resized (the ini keeps whole pixels).
            auto it = cappedDockSizes.find(child->ID);
            if (it != cappedDockSizes.end() && std::fabs(it->second.capped - sizeRef[axis]) < 1.0f) {
                uncapped = it->second.uncapped * ratio;
            }
            const float size = std::min(uncapped, std::max(sizeRef[axis], cap));
            child->Size[axis] = size;
            child->SizeRef[axis] = size;
            if (size < uncapped) {
                cappedDockSizes[child->ID] = { size, uncapped };
            } else {
                cappedDockSizes.erase(child->ID);
            }
        }
        scaleDockChildren(child, ratio);
    }
}

// The layout scale and the capped panel sizes are saved in the ini with the dock
// sizes they describe. Inis from older versions fall back to settings.yaml.
void editor::App::registerLayoutSettings(){
    ImGuiSettingsHandler handler;
    handler.TypeName = "JasperEngineLayout";
    handler.TypeHash = ImHashStr("JasperEngineLayout");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* {
        return reinterpret_cast<void*>(1);
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void*, const char* line) {
        App* app = static_cast<App*>(handler->UserData);
        ImGuiID id;
        float scale, capped, uncapped;
        if (std::sscanf(line, "Scale=%f", &scale) == 1) {
            app->layoutUiScale = scale;
        } else if (std::sscanf(line, "Panel=0x%08X Capped=%f Uncapped=%f", &id, &capped, &uncapped) == 3) {
            app->cappedDockSizes[id] = { capped, uncapped };
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        const App* app = static_cast<const App*>(handler->UserData);
        buf->appendf("[%s][Data]\n", handler->TypeName);
        buf->appendf("Scale=%g\n", sanitizeLayoutScale(app->layoutUiScale));
        for (const auto& [id, size] : app->cappedDockSizes) {
            buf->appendf("Panel=0x%08X Capped=%.1f Uncapped=%.1f\n", id, size.capped, size.uncapped);
        }
        buf->append("\n");
    };
    ImGui::AddSettingsHandler(&handler);
}

// A new UI scale (Editor Settings) resizes the docked panels along with the text.
void editor::App::applyUiScale(){
    const float uiScale = AppSettings::getUiScale();
    if (uiScale == Theme::uiScale()) return;

    // Until the layout is restored, rescaleRestoredLayout() takes care of it.
    if (layoutScaleApplied) {
        const float ratio = uiScale / Theme::uiScale();
        if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id)) {
            scaleDockChildren(root, ratio);
        }
        layoutUiScale *= ratio;
    }
    Theme::setUiScale(uiScale);
}

void editor::App::buildDefaultLayout(){
    // WorkSize is what DockSpaceOverViewport() sizes the root node to, so the
    // caps below measure against the room the panels actually get.
    const ImVec2 viewport = ImGui::GetMainViewport()->WorkSize;
    ImGuiID dock_id_left, dock_id_left_top, dock_id_right, dock_id_middle, dock_id_middle_bottom;
    // dock_id_middle_top is a member: dockTabWindow() reads it after this runs.

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport);

    // Unity's "Default" layout, as shares of the window: Inspector down the
    // whole right side, Project/Console across the bottom of the rest,
    // Hierarchy top-left and the Scene/Game tabs in the centre. Each share is
    // held to a minimum in font sizes so small windows keep readable panels.
    auto share = [](float fraction, float available, float minFontSizes){
        return std::min(std::max(fraction * available, minFontSizes * ImGui::GetFontSize()), available * panelMaxShare);
    };
    const float rightWidth = share(0.22f, viewport.x, 16);
    const float leftWidth = share(0.18f, viewport.x, 12);
    const float bottomHeight = share(0.30f, viewport.y, 10);

    // Inspector on the right, full height; MCP, Terrain and Physics Placement behind it.
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.0f, &dock_id_right, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(rightWidth, viewport.y));
    ImGui::DockBuilderSetNodeSize(dock_id_middle, ImVec2(viewport.x - rightWidth, viewport.y));
    ImGui::DockBuilderDockWindow(InspectorWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(mcp::McpActivityWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(TerrainEditWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(PhysicsPlacementWindow::WINDOW_NAME, dock_id_right);

    // Project, Console and Animation across the bottom, under Hierarchy + Scene.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Down, 0.0f, &dock_id_middle_bottom, &dock_id_left);
    ImGui::DockBuilderSetNodeSize(dock_id_middle_bottom, ImVec2(viewport.x - rightWidth, bottomHeight));
    ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(viewport.x - rightWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderDockWindow(ProjectWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(ConsoleWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(UndoHistoryWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(PlayModeChangesWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(BookmarksWindow::WINDOW_NAME, dock_id_middle_bottom);
    // Systems: an eight-column table, wide enough only here (in the Inspector's column the
    // system names and the Enabled checkboxes were cut off)
    ImGui::DockBuilderDockWindow(SystemsWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(AnimationWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(AnimatorWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(PoseAnimationWindow::WINDOW_NAME, dock_id_middle_bottom);

    // Hierarchy top-left; the Scene tabs and Game view fill the centre.
    ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Left, 0.0f, &dock_id_left_top, &dock_id_middle_top);
    ImGui::DockBuilderSetNodeSize(dock_id_left_top, ImVec2(leftWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderSetNodeSize(dock_id_middle_top, ImVec2(viewport.x - rightWidth - leftWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderDockWindow(HierarchyWindow::WINDOW_NAME, dock_id_left_top);
    ImGui::DockBuilderDockWindow("Game", dock_id_middle_top);
    // Texture Synthesis is mostly a preview: a tab next to the Scene and Game views
    ImGui::DockBuilderDockWindow(TextureSynthesisWindow::WINDOW_NAME, dock_id_middle_top);
    // the Audio Mixer's strips want the width of the views
    ImGui::DockBuilderDockWindow(AudioMixerWindow::WINDOW_NAME, dock_id_middle_top);
    // the Particle Effect window: a preview and its timing, as wide as the views
    ImGui::DockBuilderDockWindow(ParticleEffectWindow::WINDOW_NAME, dock_id_middle_top);
    // the baker too (its preview), so the Inspector entries that open it stay in view
    ImGui::DockBuilderDockWindow(VertexAnimationBakerWindow::WINDOW_NAME, dock_id_middle_top);
    // the Sprite Normal Map window: a preview too
    ImGui::DockBuilderDockWindow(SpriteNormalsWindow::WINDOW_NAME, dock_id_middle_top);
    // a graph wants the width of the views
    ImGui::DockBuilderDockWindow(GraphEditorWindow::WINDOW_NAME, dock_id_middle_top);

    // Tab order inside the shared nodes (first one is the visible tab).
    auto stampPanelDockOrder = [](const char* windowName, short order) {
        ImGuiID windowId = ImHashStr(windowName);
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName);
        settings->DockOrder = order;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName)) {
            window->DockOrder = order;
        }
    };
    stampPanelDockOrder(InspectorWindow::WINDOW_NAME, 0);
    stampPanelDockOrder(mcp::McpActivityWindow::WINDOW_NAME, 1);
    stampPanelDockOrder(TerrainEditWindow::WINDOW_NAME, 2);
    stampPanelDockOrder(PhysicsPlacementWindow::WINDOW_NAME, 3);
    stampPanelDockOrder(ProjectWindow::WINDOW_NAME, 0);
    stampPanelDockOrder(ConsoleWindow::WINDOW_NAME, 1);
    stampPanelDockOrder(AnimationWindow::WINDOW_NAME, 2);
    stampPanelDockOrder(PoseAnimationWindow::WINDOW_NAME, 3);
    stampPanelDockOrder(UndoHistoryWindow::WINDOW_NAME, 4);
    stampPanelDockOrder(PlayModeChangesWindow::WINDOW_NAME, 5);
    stampPanelDockOrder(SystemsWindow::WINDOW_NAME, 6);
    stampPanelDockOrder(AnimatorWindow::WINDOW_NAME, 7);
    stampPanelDockOrder(BookmarksWindow::WINDOW_NAME, 8);

    // Mark the scene area as the central node. CentralNode is a saved flag, so it
    // persists in the ini and lets getCentralDockId() recover this node on the
    // next launch without rebuilding the layout.
    if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(dock_id_middle_top)) {
        centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
    }
}

void editor::App::dockProjectTabs(){
    // Re-open document tabs and dock scenes in their saved order. dockTabWindow()
    // leaves already-known windows where the ini put them (unless resetting).
    const std::vector<TabEntry>& tabs = project.getTabs();

    auto dockSceneTab = [&](const TabEntry& tab) {
        for (auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                addNewSceneToDock(sceneProject.id);
                break;
            }
        }
    };

    auto dockCodeTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!codeEditor->isFileOpen(tab.filepath)) {
                codeEditor->openFile(tab.filepath);
            } else {
                dockTabWindow("###" + tab.filepath);
            }
        }
    };

    auto dockImageTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!imageViewerWindow->isFileOpen(tab.filepath)) {
                imageViewerWindow->openFile(tab.filepath, false);
            } else {
                dockTabWindow(ImageViewerWindow::getWindowId(tab.filepath));
            }
        }
    };

    auto dockOrphanScenes = [&]() {
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                addNewSceneToDock(sceneProject.id);
            }
        }
    };

    if (forceDockTabs) {
        // Reset layout: scenes first, then code and image documents.
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
        }
        dockOrphanScenes();
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::IMAGE_VIEWER) dockImageTab(tab);
        }
    } else {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
            else if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
            else if (tab.type == TabType::IMAGE_VIEWER) dockImageTab(tab);
        }
        dockOrphanScenes();
    }

    // Make the saved tab list authoritative for tab ordering. Scene window ids
    // (###Scene<id>) are reassigned on every load, so the per-window DockOrder
    // ImGui keeps in its ini can't be matched back to a scene. Stamp a sequential
    // DockOrder onto each tab's window settings from project.tabs order so the
    // shared central node restores its tabs in the order the user left them
    // (captured live by captureTabOrder()). ImGui copies this onto the window's
    // DockOrder when it is created, and tabs appearing on the same frame are
    // sorted by it.
    short dockOrder = 0;
    auto stampDockOrder = [&](const std::string& windowName) {
        if (windowName.empty()) return;
        ImGuiID windowId = ImHashStr(windowName.c_str());
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName.c_str());
        settings->DockOrder = dockOrder;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName.c_str())) {
            window->DockOrder = dockOrder;
        }
        dockOrder++;
    };

    if (forceDockTabs) {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) stampDockOrder(tabWindowName(tab));
        }
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                stampDockOrder("###Scene" + std::to_string(sceneProject.id));
            }
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) stampDockOrder(tabWindowName(tab));
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::IMAGE_VIEWER) stampDockOrder(tabWindowName(tab));
        }
    } else {
        for (const auto& tab : tabs) {
            stampDockOrder(tabWindowName(tab));
        }
    }
}

std::string editor::App::tabWindowName(const TabEntry& tab) const {
    if (tab.type == TabType::SCENE) {
        for (const auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                return "###Scene" + std::to_string(sceneProject.id);
            }
        }
        return {};
    }
    if (tab.type == TabType::IMAGE_VIEWER) {
        return ImageViewerWindow::getWindowId(tab.filepath);
    }
    // CodeEditor docks its windows as "###<relative-filepath>" (see getWindowTitle()).
    return "###" + tab.filepath;
}

void editor::App::captureTabOrder() {
    // Mirror the live ImGui tab order back into project.tabs so a user's
    // drag-reordering of scene/code/image tabs survives a save and the next launch.
    // ImGui keeps each window's visual position in its DockNode as DockOrder;
    // we reorder the (filepath-keyed, stable) tab list to match. Reordering a
    // tab triggers no save on its own, so we persist the change ourselves,
    // debounced until the user stops dragging.
    std::vector<TabEntry>& tabs = project.getTabs();
    if (tabs.size() >= 2) {
        struct OrderedTab { TabEntry entry; int order; };
        std::vector<OrderedTab> ordered;
        ordered.reserve(tabs.size());
        for (const TabEntry& tab : tabs) {
            ImGuiWindow* window = ImGui::FindWindowByName(tabWindowName(tab).c_str());
            // Windows that aren't currently docked (DockOrder < 0) or not yet
            // instantiated keep their relative position via the stable sort.
            int order = (window && window->DockOrder >= 0) ? window->DockOrder
                                                           : std::numeric_limits<int>::max();
            ordered.push_back({tab, order});
        }

        std::stable_sort(ordered.begin(), ordered.end(),
            [](const OrderedTab& a, const OrderedTab& b){ return a.order < b.order; });

        bool changed = false;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].type != ordered[i].entry.type || tabs[i].filepath != ordered[i].entry.filepath) {
                tabs[i] = ordered[i].entry;
                changed = true;
            }
        }

        if (changed) {
            tabsOrderDirty = true;
            tabsOrderChangeTime = ImGui::GetTime();
        }
    }

    // Persist a short moment after the last reorder so a drag results in one
    // write rather than one per frame while the tab slides past its neighbours.
    if (tabsOrderDirty && ImGui::GetTime() - tabsOrderChangeTime > 0.75) {
        project.saveWorkspaceFile();
        tabsOrderDirty = false;
    }
}

void editor::App::dockTabWindow(const std::string& windowName, bool force){
    // Auto-dock only a window the layout ini has never seen, so we never override
    // a position the user chose (docked or intentionally floating). A layout reset
    // (forceDockTabs) and explicit user opens (force) snap the tab to the centre.
    if (force || forceDockTabs || !ImGui::FindWindowSettingsByID(ImHashStr(windowName.c_str()))) {
        dock_id_middle_top = getCentralDockId();
        ImGui::DockBuilderDockWindow(windowName.c_str(), dock_id_middle_top);
    }
}

ImGuiID editor::App::getCentralDockId(){
    // The scene area carries ImGuiDockNodeFlags_CentralNode (a saved flag, so it
    // survives in the ini). We scan for it rather than calling
    // DockBuilderGetCentralNode(), which reads root->CentralNode — only populated
    // once DockSpace() has processed the dockspace, not yet true on the first
    // frame after the layout is restored from the ini.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    for (const ImGuiStoragePair& pair : ctx->DockContext.Nodes.Data) {
        ImGuiDockNode* node = static_cast<ImGuiDockNode*>(pair.val_p);
        if (node && node->IsCentralNode() && ImGui::DockNodeGetRootNode(node)->ID == dockspace_id) {
            return node->ID;
        }
    }

    // No central node yet (e.g. a pre-existing ini without one): fall back to the
    // dockspace root so windows still attach somewhere sensible.
    return dockspace_id;
}

ImFont* editor::App::getCodeFont() {
    return codeFont ? codeFont : ImGui::GetIO().Fonts->Fonts[0];
}

ImFont* editor::App::getBoldFont() {
    return boldFont ? boldFont : ImGui::GetFont();
}

void editor::App::setup() {
    // DontDestroyOnLoad moves nothing in edit mode (Play installs the session's factory)
    Project::refusePersistentScenes();
    mainThreadId = std::this_thread::get_id();

    // a queued offscreen pose render keeps the loop drawing until processFrame serves it
    PoseRender::setFrameRequester([this]() { requestRedraw(); });
    ParticleEffectPreview::setFrameRequester([this]() { requestRedraw(); });
    procmat::ProceduralRun::setFrameRequester([this]() { requestRedraw(); });

    // Initialize application settings
    initializeSettings();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    // Activate only where it works: on Wayland the backend can't reposition windows,
    // so the feature stays off this session until a restart forces the X11 backend.
    if (AppSettings::getMultiViewportEnabled() && !Backend::isRunningOnWayland())
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Multi-Viewport / Platform Windows (toggle in View menu)

    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // math in every numeric field (2*1.5, 25cm, +=10): libs/imgui/JASPER_PATCHES.md
    NumericExpression::installImGuiHook();
    // scripts: the compile status the toolbar badge shows, the reload of what is saved while
    // a scene plays, and the toolchain the C++ script libraries must match (S23)
    ScriptStatus::get().followBuilds();
    ScriptReloadService::get().attach(&project);
    checkEditorEngineFingerprint();

    // The window layout is the project's working layout (.jasperengine/user/layout.yaml), or
    // without a saved project editor_layout_v2.ini next to settings.yaml: LayoutManager reads
    // and writes it, so ImGui's own ini file is off (S25 R19). At most one write a second.
    io.IniFilename = nullptr;
    io.IniSavingRate = 1.0f;
    // the Animation window's saved label width and view: an ImGui settings handler, in the
    // layout's ini text (before the first layout loads)
    AnimationWindowClip::registerSettingsHandler();
    layouts.loadGlobalIni();
    setupLayouts();
    // the command catalog and the user's rebindings (shortcuts.yaml, next to settings.yaml)
    registerCommands();

    // Separate from the window scale: the layout keeps the scale it was built at,
    // while the window is measured on whichever monitor it closes on.
    layoutUiScale = AppSettings::getLayoutUiScale();

    io.Fonts->AddFontDefault();

    ImFontConfig config;
    config.MergeMode = true;
    config.FontDataOwnedByAtlas = false;
    // Merge size 0.0f: inherit the implicit reference size of AddFontDefault().
    // Since 1.92, merging with an explicit size into an implicit-ref-size font asserts,
    // and specifying GlyphMinAdvanceX (monospacing) requires a non-zero reference size,
    // so it is omitted here. The visible icon font is font1 below, which keeps monospacing.
    static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 0.0f, &config, icon_ranges);

    // Unity's UI text is small and dense; 15px Roboto gets close while staying
    // crisp. Icons merge at the same size so they line up with the text.
    const float uiFontSize = 15.0f;

    ImFontConfig config1;
    strcpy(config1.Name, "roboto-regular (15 px)");
    config1.FontDataOwnedByAtlas = false;
    config1.OversampleH = 2;
    config1.OversampleV = 2;
    config1.RasterizerMultiply = 1.5f;
    ImFont* font1 = io.Fonts->AddFontFromMemoryTTF(roboto_regular_ttf, roboto_regular_ttf_len, uiFontSize, &config1);

    ImFontConfig config2;
    config2.MergeMode = true;
    config2.GlyphMinAdvanceX = uiFontSize; // Use if you want to make the icon monospaced
    config2.FontDataOwnedByAtlas = false;
    static const ImWchar icon_ranges2[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, uiFontSize, &config2, icon_ranges2);

    // Same Arabic subset the engine renders text with, so property fields and the scene
    // tree show Arabic instead of missing-glyph boxes. Since 1.92 the atlas loads glyphs
    // on demand, it only needs a font that carries them.
    ImFontConfig configArabic;
    configArabic.MergeMode = true;
    configArabic.FontDataOwnedByAtlas = false;
    //same rasterizer settings as the font it merges into, or Arabic comes out thinner
    //and fainter than the Latin beside it
    configArabic.OversampleH = 2;
    configArabic.OversampleV = 2;
    configArabic.RasterizerMultiply = 1.5f;
    io.Fonts->AddFontFromMemoryTTF(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, uiFontSize, &configArabic);

    // The same, bold, with the same icons (the Inspector's overridden prefab values)
    ImFontConfig configBold;
    strcpy(configBold.Name, "roboto-bold (15 px)");
    configBold.FontDataOwnedByAtlas = false;
    configBold.OversampleH = 2;
    configBold.OversampleV = 2;
    configBold.RasterizerMultiply = 1.5f;
    boldFont = io.Fonts->AddFontFromMemoryTTF(roboto_bold_ttf, roboto_bold_ttf_len, uiFontSize, &configBold);
    ImFontConfig configBoldIcons = config2;
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, uiFontSize, &configBoldIcons, icon_ranges2);
    io.Fonts->AddFontFromMemoryTTF(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, uiFontSize, &configArabic);

    ImFontConfig config3;
    strcpy(config3.Name, "jetbrains-mono-regular");
    config3.FontDataOwnedByAtlas = false;
    config3.OversampleH = 2;
    config3.OversampleV = 2;
    codeFont = io.Fonts->AddFontFromMemoryTTF(jetbrains_mono_regular_ttf, jetbrains_mono_regular_ttf_len, 16.0f, &config3);

    //scripts can hold Arabic string literals
    ImFontConfig configCodeArabic;
    configCodeArabic.MergeMode = true;
    configCodeArabic.FontDataOwnedByAtlas = false;
    configCodeArabic.OversampleH = 2;
    configCodeArabic.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, 16.0f, &configCodeArabic);

    // the system's CJK and emoji fonts, merged into these when a background search (started
    // with the job system, engineInit) has found them (applySystemFonts): CJK names and text
    // show as glyphs, and the start is not slower
    systemFontTargets = {{font1, uiFontSize, config1.RasterizerMultiply},
                         {boldFont, uiFontSize, configBold.RasterizerMultiply},
                         {codeFont, 16.0f, config3.RasterizerMultiply}};

    io.FontDefault = font1;

    io.ConfigDragClickToInputText = true;

    Theme::apply();

    // Fonts follow each viewport's monitor DPI. Style paddings are global, so
    // App::show() re-scales them from the main viewport when that DPI changes.
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    float dpiScale = 1.0f;
    if (ImGui::GetPlatformIO().Monitors.Size > 0) {
        dpiScale = ImGui::GetPlatformIO().Monitors[0].DpiScale;
    }
    Theme::setUiScale(AppSettings::getUiScale());
    Theme::applyDpiScale(dpiScale);
}

bool editor::App::canEditSelection(bool duplicate) {
    // Enabled items keep their native key equivalent, stealing it from a focused text field.
    if (ImGui::GetIO().WantTextInput) return false;
    // the focused Animation window owns Delete / Backspace / Ctrl+D (AnimationClipAppHooks::takesKeys):
    // covers the scene key block and the native menu key equivalents
    if (animationClipAppHooks().takesKeys && animationClipAppHooks().takesKeys()) return false;
    if (lastFocusedWindow != LastFocusedWindow::AnySceneWindow || project.isAnyScenePlaying()) return false;
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t targetSceneId = sceneId;
    const uint32_t propertiesSceneId = project.getSelectedSceneForProperties();
    if (!duplicate && propertiesSceneId != NULL_PROJECT_SCENE &&
        (propertiesSceneId == sceneId || project.hasChildScene(sceneId, propertiesSceneId))) {
        targetSceneId = propertiesSceneId;
    }
    SceneProject* scene = project.getScene(targetSceneId);
    if (!scene) return false;
    if (scene->sceneRender && (scene->sceneRender->getSelectedTileIndex() >= 0 ||
        (!duplicate && scene->sceneRender->getSelectedInstanceIndex() >= 0))) return true;
    if (!project.getSelectedEntities(targetSceneId).empty()) return true;
    return !duplicate && propertiesSceneId != NULL_PROJECT_SCENE && propertiesSceneId != sceneId &&
        project.hasChildScene(sceneId, propertiesSceneId);
}

void editor::App::deleteSelection() {
    if (!canEditSelection(false)) return;
    // a posed character goes back to its rest bones first, so they are what the undo keeps
    PoseEditSession::suspendActive();
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t selectedSceneForProperties = project.getSelectedSceneForProperties();
    uint32_t targetSceneId = sceneId;
    if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
        (selectedSceneForProperties == sceneId || project.hasChildScene(sceneId, selectedSceneForProperties))) {
        targetSceneId = selectedSceneForProperties;
    }

    // Check if a tile is selected — delete it instead of the entity
    SceneProject* sp = project.getScene(targetSceneId);
    bool tileDeleted = false;
    bool instanceDeleted = false;
    if (sp && sp->sceneRender) {
        // a polyline point selected in the Scene view: that point goes, not the GameObject
        if (sp->sceneRender->removeSelectedLinePoint(&project, targetSceneId)) {
            return;
        }
        int tileIdx = sp->sceneRender->getSelectedTileIndex();
        Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
        if (tileIdx >= 0) {
            Command* deleteCmd = ProjectUtils::buildDeleteTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
            if (deleteCmd) {
                CommandHandle::get(sceneId)->addCommand(deleteCmd);
                sp->sceneRender->clearTileSelection();
                tileDeleted = true;
            }
        }

        int instIdx = sp->sceneRender->getSelectedInstanceIndex();
        Entity instEntity = sp->sceneRender->getSelectedInstanceEntity();
        if (!tileDeleted && instIdx >= 0) {
            Command* deleteCmd = ProjectUtils::buildDeleteInstanceCmd(&project, targetSceneId, instEntity, (unsigned int)instIdx);
            if (deleteCmd) {
                CommandHandle::get(sceneId)->addCommand(deleteCmd);
                sp->sceneRender->clearInstanceSelection();
                instanceDeleted = true;
            }
        }
    }

    if (!tileDeleted && !instanceDeleted) {
        const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);

        Command* lastCmd = nullptr;
        if (!selectedEntities.empty()) {
            // One command for the whole selection: one snapshot pass and one undo step
            lastCmd = new DeleteEntityCmd(&project, targetSceneId, selectedEntities);
            CommandHandle::get(sceneId)->addCommand(lastCmd);
        } else {
            if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
                selectedSceneForProperties != sceneId &&
                project.hasChildScene(sceneId, selectedSceneForProperties)) {
                    lastCmd = new RemoveChildSceneCmd(&project, sceneId, selectedSceneForProperties);
                    CommandHandle::get(sceneId)->addCommand(lastCmd);
            }
        }
        if (lastCmd) {
            lastCmd->setNoMerge();
        }
    }
}

void editor::App::duplicateSelection() {
    if (!canEditSelection(true)) return;
    // a posed character goes back to its rest bones first, so they are what gets copied
    PoseEditSession::suspendActive();
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t targetSceneId = sceneId;
    SceneProject* sp = project.getScene(targetSceneId);
    bool tileDuplicated = false;
    if (sp && sp->sceneRender) {
        int tileIdx = sp->sceneRender->getSelectedTileIndex();
        Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
        if (tileIdx >= 0) {
            Command* dupCmd = ProjectUtils::buildDuplicateTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
            if (dupCmd) {
                CommandHandle::get(sceneId)->addCommand(dupCmd);
                TilemapComponent* tilemap = sp->scene->findComponent<TilemapComponent>(tileEntity);
                if (tilemap) {
                    sp->sceneRender->selectTile(tileEntity, (int)tilemap->numTiles - 1);
                }
                tileDuplicated = true;
            }
        }
    }
    if (!tileDuplicated) {
        const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);
        if (!selectedEntities.empty()){
            CommandHandle::get(sceneId)->addCommandNoMerge(new DuplicateEntityCmd(&project, targetSceneId, selectedEntities));
        }
    }
}

bool editor::App::isSceneEditContext() const {
    // Enabled items keep their native key equivalent, stealing it from a focused text field
    return !ImGui::GetIO().WantTextInput && lastFocusedWindow == LastFocusedWindow::AnySceneWindow;
}

bool editor::App::canSelectAll() {
    SceneProject* scene = project.getSelectedScene();
    return isSceneEditContext() && scene && scene->scene && !scene->entities.empty();
}

bool editor::App::canCutSelection() {
    // Deleting follows Duplicate: not while a scene plays
    return canCopySelection() && !project.isAnyScenePlaying();
}

bool editor::App::canCopySelection() {
    // Copying only reads, so it also works in Play mode (to paste the values after Stop)
    SceneProject* scene = project.getSelectedScene();
    return isSceneEditContext() && scene && scene->scene && !scene->selectedEntities.empty();
}

bool editor::App::canPasteSelection(bool asChild) {
    SceneProject* scene = project.getSelectedScene();
    if (!isSceneEditContext() || project.isAnyScenePlaying() || !scene || !scene->scene ||
        !EntityClipboard::hasGameObjects(&project)) {
        return false;
    }
    if (!asChild) {
        return true;
    }
    for (Entity entity : scene->selectedEntities) {
        if (scene->scene->isEntityCreated(entity) && scene->scene->findComponent<Transform>(entity)) {
            return true;
        }
    }
    return false;
}

void editor::App::selectAll() {
    if (!canSelectAll()) return;
    SceneProject* scene = project.getSelectedScene();
    // in the Scene view, a selected lattice's points (its own Select All)
    if (sceneWindow->isFocused() && LatticeSceneTools::selectAllPoints(&project, scene->id)) return;
    // A child is selected through its parent, as in the Hierarchy
    project.replaceSelectedEntities(scene->id, Project::getTopLevelEntities(scene->scene, scene->entities));
}

void editor::App::copySelection(bool cut) {
    if (!(cut ? canCutSelection() : canCopySelection())) return;
    // a posed character goes back to its rest bones first, so they are what gets copied
    PoseEditSession::suspendActive();
    const uint32_t sceneId = project.getSelectedSceneId();
    std::vector<Entity> selection = EntityClipboard::getCopyableSelection(&project, sceneId);
    if (selection.empty()) return;
    if (cut) {
        EntityClipboard::cutGameObjects(&project, sceneId, selection);
    } else {
        EntityClipboard::copyGameObjects(&project, sceneId, selection);
    }
}

void editor::App::pasteSelection(bool asChild) {
    if (!isSceneEditContext() || project.isAnyScenePlaying()) return;
    SceneProject* scene = project.getSelectedScene();
    if (!scene || !scene->scene) return;
    // Reads the clipboard itself: another editor instance may have copied since the last probe
    EntityClipboard::pasteGameObjects(&project, scene->id, asChild);
}

void editor::App::show(){
    applyUiScale();

    float dpiScale = 1.0f;
    if (const ImGuiViewport* mainViewport = ImGui::GetMainViewport()) {
        dpiScale = mainViewport->DpiScale;
    }
    Theme::applyDpiScale(dpiScale);

    if (projectLoading) {
        if (isInitialized) {
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
        }
        loadingWindow->show();
        return;
    }

    // S27: the Project Hub alone (as the loading window above): the editor's windows wait
    updateHubCreation();
    if (projectHub.pollSweep()) requestRedraw();
    if (projectHub.isOpen()) {
        if (isInitialized) {
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
        }
        projectHub.show();
        UIProbe::flush();   // the Inspector flushes it otherwise, and it is not drawn here
        if (hubCreation) {
            ImGui::OpenPopup("Creating Project##hub");
        }
        if (EditorModal::begin("Creating Project##hub", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove, EditorModal::Role::Progress)) {
            if (!hubCreation) ImGui::CloseCurrentPopup();
            else ImGui::Text("Creating %s from %s...", hubCreation->name.c_str(), hubCreation->info.name.c_str());
            ImGui::EndPopup();
        }
        if (!hubMessage.empty()) {
            registerAlert("Project Hub", hubMessage);
            hubMessage.clear();
        }
        showAlert();
        mcp::McpServer::get().tick();
        return;
    }

    // the spline tools: Live Terrain's re-bake, a spline that went away (S18 R12)
    SplineEditor::get().update(&project);

    EntityClipboard::update();

    pollExternalChanges();

    // Scripts ran in engineRender, so drop what they destroyed before any window reads the selection
    for (SceneProject& sceneProject : project.getScenes()) {
        const bool running = sceneProject.playState == ScenePlayState::PLAYING || sceneProject.playState == ScenePlayState::PAUSED;
        if (running && sceneProject.scene) {
            project.deselectDestroyedEntities(&sceneProject);
        }
    }
    // and the same for the running copies of child scenes, which also go when the game drops them
    project.syncPlayRuntimeSelections();

    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        // the Jobs & Memory panel shows the threading settings: its undo is the project's.
        // A Physics Placement gesture of the selected scene keeps the keys on the scene while it
        // runs (the timeline's undo cancels it first: CommandHistory::beforeChange)
        const bool placementBusy = physicsPlacementWindow->isBusy(project.getSelectedSceneId());
        // the Animator, Particle Effect and graph windows edit project assets (their selection in the Inspector too)
        if (projectWindow->isFocused() || jobsMemoryWindow->isFocused() || animatorWindow->isFocused() || particleEffectWindow->isFocused() ||
                GraphEditorWindow::focused() ||
                (focusedWindowName() == InspectorWindow::WINDOW_NAME &&
                 (animatorWindow->inspectorShowsSelection() || GraphEditorWindow::anyInspectorShowsSelection() || inspectorWindow->showsProjectAsset()))) {
            lastFocusedWindow = LastFocusedWindow::Resources;
        } else if (codeEditor->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::Code;
        } else if (spriteNormalsWindow->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::SpriteNormals;
        } else if (poseAnimationWindow->ownsUndo(sceneWindow->isFocused())) {
            lastFocusedWindow = LastFocusedWindow::PoseAnimation;
        } else if (clipUndoGoesToProject(placementBusy)) {
            lastFocusedWindow = LastFocusedWindow::AnimationClip;
        }else{
            lastFocusedWindow = LastFocusedWindow::AnySceneWindow;
        }
        PanelClaims::editor().refresh();
    }

    // the selection changes of one frame are one undo step
    project.nextSelectionFrame();

    handleInteractiveEdits(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

    // Every editor key binding: the command catalog's one dispatcher (S25 R3). Local keys
    // (Esc in dialogs, the Console's arrows, the text editors, Play input) stay with their windows.
    processShortcuts();

    if (isDroppedExternalPaths) {
        isDroppedExternalPaths = false;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            std::vector<char> payload = Util::encodeStringsPayload(droppedExternalPaths);
            ImGui::SetDragDropPayload("external_files", payload.data(), payload.size());
            ImGui::EndDragDropSource();
        }
    }

    dockspace_id = ImGui::GetID("MyDockspace");

    // the version control snapshot and the diffs before the windows read them (S29)
    vcsUi->update();
    showMenu();
    EditorDiagnostics::pollNotices();
    showCrashNotice();
    mainToolbar->show();
    statusBar->show();
    showFooter();
    showHangToast();

    isInitialized = true;

    // a layout applied between frames: its panels, central node, scale and documents
    layouts.inFrame();
    // the asset labels a background read found (S25)
    project.getAssetLabels().update();

    // Before DockSpaceOverViewport() below lays the restored sizes out.
    rescaleRestoredLayout();

    if (dockspaceNeedsRebuild || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDockspace();
        dockspaceNeedsRebuild = false;
    }

    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    showAlert();

    sceneSaveDialog.show();
    // the importers' edits not applied, asked about before a selection change, Play or the close
    ImporterPrompt::draw();
    projectSaveDialog.show();
    dataSafetyDialogs.show(project);
    exportWindow.show();
    if (exportWindow.consumePlayerSettingsRequest()) projectSettingsWindow.open(&project, "Player");
    if (inspectorWindow->consumeTagsAndLayersRequest()) projectSettingsWindow.open(&project, "Tags and Layers");
    if (inspectorWindow->consumeShaderGlobalsRequest()) projectSettingsWindow.open(&project, "Graphics");
    editorSettingsWindow.show();
    aboutWindow.show();
    projectSettingsWindow.show();
    nativePluginDialog.show();
    // a native plugin was made (either dialog): its Lua mock opens in the code editor
    for (const fs::path& created : {nativePluginDialog.consumeCreatedFile(), projectSettingsWindow.consumeCreatedFile()}) {
        if (created.empty()) continue;
        projectWindow->refreshCurrentDirectory();
        codeEditor->openFile(created.string());
    }
    prefabsWindow.show();
    scenesWindow.show();

    if (!saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen() && !saveDialogQueue.empty()) {
        processNextSaveDialog();
    }

    hierarchyWindow->show();
    projectWindow->show();
    imageViewerWindow->show();
    consoleWindow->show();
    animationWindow->show();
    animatorWindow->show();
    avatarWindow->show();
    HumanoidConvertDialog::draw(&project);
    poseAnimationWindow->show();
    // the Scene views posing its character: a selection elsewhere leaves posing, ghosts go
    PoseViewport::frame(&project);
    terrainEditWindow->show();
    textureSynthesisWindow->show();
    audioMixerWindow->show();
    particleEffectWindow->show();
    soundBrowserWindow->show();
    audioCapturesWindow->show();
    // the Graph Editor and every graph kind's own window
    GraphEditorWindow::showAll();
    // also the tick of its Shift-click generations (with the window closed)
    spriteNormalsWindow->show();
    // also the tick of a running bake (a Shift-click bake runs with the window closed)
    vertexAnimationBakerWindow->show();
    // also the tick of the Scene view's physics sessions, with the panel closed too
    physicsPlacementWindow->show();
    inspectorWindow->show();
    mcpActivityWindow->show();
    systemsWindow->show();
    missingAssetsWindow->show();
    assetReferencesWindow->show();
    jobsMemoryWindow->show();
    meshAuditWindow->show();
    buildReportWindow->show();
    profilerWindow->show();
    frameDebuggerWindow->show();
    undoHistoryWindow->show();
    vcsUi->show();
    inputActionsWindow->show();
    navigationWindow->show();
    playModeChangesWindow->show();
    bookmarksWindow->show();
    codeEditor->show();
    gameWindow->show();
    // Prefab Mode's tab docks with the scene tabs the first time a stage shows
    if (project.getActivePrefabStage()) {
        dockTabWindow(SceneWindow::PREFAB_STAGE_WINDOW_ID);
    }
    sceneWindow->show();
    Backend::setGameCursorInSceneRect(gameWindow->isGameCursorInView());

    drawSaveProjectAsTemplate();

    // S27: the Recorder's session and window
    recorder::RecorderSession::get().update();
    recorderWindow.setProjectPath(project.getProjectPath());
    recorderWindow.show();
    if (isRecording()) requestRedraw();

    // S27: the field windows after every window with a field (a field takes its window's
    // changes on its next draw, before the window ends a session); the Gradient Editor's colour
    // keys open the Color window, so it comes last
    ui::CurveWindow::get().setProjectPath(project.getProjectPath());
    ui::CurveWindow::get().show();
    ui::GradientWindow::get().show();
    ui::ColorPickerWindow::get().setProjectPath(project.getProjectPath());
    ui::ColorPickerWindow::get().show();

    loadingWindow->show();

    // an expression typed into a numeric field that did not evaluate
    NumericExpression::drawErrorTooltip();

    // Keep the persisted tab list in sync with the live tab order so a user's
    // drag-reordering of scene/code tabs is saved (and restored on next launch).
    captureTabOrder();

    // Closing a panel through its title-bar button changes the same state as the
    // View menu, so mirror the live state after all panels have rendered.
    persistPanelVisibilitySettings();

    // S25: the Search palette, the Shortcuts window, the layout dialogs, over everything
    searchPalette->show();
    shortcutsWindow->show();
    layouts.drawDialogs();
    layouts.endFrame();

    LoudnessMonitor::get().update(project.isAnyScenePlaying());

    // MCP calls waiting on a condition or on frames (waitUntil / afterFrames)
    mcp::McpServer::get().tick();
}

void editor::App::setLoadingPump(std::function<void(bool)> pump) {
    loadingPump = std::move(pump);
}

void editor::App::reportLoadingProgress(const std::string& status) {
    if (!isMainThread() || !projectLoading || pumpingLoading) return;
    // a load that keeps reporting progress is alive
    diag::heartbeat();
    if (!status.empty()) loadingStatus = status;
    if (!loadingPump) return;

    const auto now = std::chrono::steady_clock::now();
    if (lastLoadingFrame.time_since_epoch().count() != 0 &&
        now - lastLoadingFrame < std::chrono::milliseconds(33)) return;
    lastLoadingFrame = now;

    // Project loading stays on the main thread. Pump only native events and the
    // loading UI here; editor tasks must wait until the project is complete.
    pumpingLoading = true;
    try {
        loadingPump(Engine::isViewLoaded());
    } catch (...) {
        pumpingLoading = false;
        throw;
    }
    pumpingLoading = false;
}

static std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                else
                    out << c;
        }
    }
    return out.str();
}

void editor::App::parseBenchmarkArgs(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "benchmark") != 0)
        return;

    benchmarkEnabled = true;
    auto requireValue = [&](int& index, const char* name) -> const char* {
        if (index + 1 >= argc) {
            failBenchmark(std::string("missing value for ") + name);
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-p" || arg == "--project") {
            if (const char* value = requireValue(i, "--project"))
                benchmarkProject = CommandLine::projectArgPath(value);
        } else if (arg == "-s" || arg == "--scene") {
            if (const char* value = requireValue(i, "--scene"))
                benchmarkScene = value;
        } else if (arg == "--frames") {
            if (const char* value = requireValue(i, "--frames"))
                benchmarkMeasureFrames = std::max(1, std::atoi(value));
        } else if (arg == "--warmup") {
            if (const char* value = requireValue(i, "--warmup"))
                benchmarkWarmupFrames = std::max(0, std::atoi(value));
        } else if (arg == "--mesh-lod") {
            if (const char* value = requireValue(i, "--mesh-lod"))
                benchmarkMeshLod = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--gpu-instancing") {
            // the project's GPU Instancing for this run (not saved): S55's before and after
            if (const char* value = requireValue(i, "--gpu-instancing"))
                benchmarkGpuInstancing = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--depth-prepass") {
            if (const char* value = requireValue(i, "--depth-prepass"))
                benchmarkDepthPrepass = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--lit-quality") {
            if (const char* value = requireValue(i, "--lit-quality")) {
                if (std::strcmp(value, "standard") == 0) benchmarkLitQuality = 0;
                else if (std::strcmp(value, "high") == 0) benchmarkLitQuality = 1;
                else failBenchmark(std::string("--lit-quality must be standard or high, not ") + value);
            }
        } else if (arg == "--play") {
            benchmarkPlay = true;
        } else if (arg == "--name") {
            if (const char* value = requireValue(i, "--name"))
                benchmarkName = value;
        } else if (arg == "--out") {
            if (const char* value = requireValue(i, "--out"))
                benchmarkOut = CommandLine::absoluteArgPath(value);
        } else if (arg == "--ui-stats") {
            benchmarkUiStats = true;
        } else if (arg == "--gpu-passes") {
            benchmarkGpuPasses = true;
        } else if (arg == "--profile") {
            benchmarkProfile = true;
        } else if (arg == "--profile-capture" || arg.rfind("--profile-capture=", 0) == 0) {
            const char* value = arg.size() > 17 ? argv[i] + 18 : requireValue(i, "--profile-capture");
            if (value && value[0])
                benchmarkCapturePath = CommandLine::absoluteArgPath(value);
            else if (value)
                failBenchmark("--profile-capture needs a file path");
        } else if (arg == "-h" || arg == "--help") {
            continue;
        } else {
            failBenchmark("unknown argument '" + arg + "'");
            return;
        }
    }

    if (benchmarkProject.empty()) {
        failBenchmark("--project is required");
    }
}

// the project's start scene unless --scene names another; true once it is the selected one
bool editor::App::selectBenchmarkScene() {
    // a scene file: relative to the working directory (as every path argument), else to the project
    fs::path scenePath;
    if (!benchmarkScene.empty()) {
        std::error_code ec;
        scenePath = CommandLine::absoluteArgPath(benchmarkScene);
        if (fs::path(benchmarkScene).is_relative() && !fs::is_regular_file(scenePath, ec))
            scenePath = CommandLine::absoluteArgPath(project.getProjectPath() / benchmarkScene);
    }

    SceneProject* match = nullptr;
    for (SceneProject& sceneProject : project.getScenes()) {
        const bool wanted = benchmarkScene.empty()
            ? sceneProject.id == project.getStartSceneId()
            : (sceneProject.name == benchmarkScene || sceneProject.filepath.filename() == benchmarkScene ||
               (!sceneProject.filepath.empty() &&
                CommandLine::absoluteArgPath(project.getProjectPath() / sceneProject.filepath) == scenePath));
        if (wanted) {
            match = &sceneProject;
            break;
        }
    }
    if (!match)
        return false;

    benchmarkSceneId = match->id;
    if (!match->opened)
        project.openScene(match->filepath, false);
    else
        project.setSelectedSceneId(benchmarkSceneId);

    SceneProject* selected = project.getSelectedScene();
    return selected && selected->id == benchmarkSceneId;
}

void editor::App::failBenchmark(const std::string& message) {
    std::cerr << "benchmark: " << message << "\n";
    benchmarkFailed = true;
    benchmarkExit = true;
}

bool editor::App::consumeBenchmarkExit() {
    const bool exit = benchmarkExit;
    benchmarkExit = false;
    return exit;
}

void editor::App::engineInit(int argc, char** argv) {
    parseBenchmarkArgs(argc, argv);
    reportLoadingProgress("Initializing engine...");
    // the editor's loop marks the frames and publishes the off-thread GPU creations (EditorFrame::run,
    // BackendMac's own loop on macOS), not each Engine::systemDraw
    FrameTimes::setExternalFrames(true);
    // the profiler's command line (--profile, --profile-capture, --profile-frames), wherever it
    // was given (CommandLine::takeProfilerFlags); a benchmark captures its measured frames itself
    std::vector<char*> engineArgs(argv, argv + argc);
    std::vector<std::string> profilerFlags = benchmarkEnabled ? std::vector<std::string>() : CommandLine::takenProfilerFlags();
    for (std::string& flag : profilerFlags)
        engineArgs.push_back(flag.data());
    Engine::systemInit((int)engineArgs.size(), engineArgs.data(), new editor::Platform(&project));
    // the system's CJK and emoji fonts, looked for on the jobs' Background lane (setup's targets)
    SystemFonts::startSearch(SystemFonts::fromSettingName(AppSettings::getCjkFontLanguage()));

    Engine::pauseGameEvents(true);

    // Any edit/undo/redo flags its scene for redraw, so the loop can idle otherwise.
    // a scene undo / redo while a character is posed runs on its rest bones (PoseEditSession)
    CommandHistory::beforeStep = [](size_t sceneId){
        PoseEditSession::suspendActiveForScene(static_cast<uint32_t>(sceneId));
    };
    CommandHistory::onSceneModified = [this](size_t sceneId, bool structural){
        if (SceneProject* sceneProject = project.getScene((uint32_t)sceneId)){
            sceneProject->needUpdateRender = true;
            // the next change after a command is newer than the command's (Changed<T>)
            if (sceneProject->scene){
                sceneProject->scene->advanceChangeTick();
            }
            // Only structural commands, so a gizmo drag doesn't rebuild the tree each frame.
            if (structural){
                sceneProject->structureVersion++;
            }
        }
        // a prefab stage's Auto Save writes after its changes
        project.notePrefabStageChange((uint32_t)sceneId);
    };
    // An undo puts back the modified flag of before its step; a save since wrote the step to
    // the file, so the scene now differs from it (and the next save writes the undone state)
    CommandHistory::onUndoPastSave = [this](size_t sceneId){
        if (SceneProject* sceneProject = project.getScene((uint32_t)sceneId)){
            sceneProject->isModified = true;
        }
    };
    // A drag on a widget is one undo step (S24 R11): its first command opens the edit; a
    // command the history drops waits until no widget may still point at it
    CommandHistory::deferReleases = true;
    CommandHistory::beforeRecord = [this](CommandHistory& history){
        if (UndoOriginScope::current().kind != UndoOriginKind::User || !ImGui::GetCurrentContext()) {
            return;
        }
        const ImGuiID active = ImGui::GetActiveID();
        if (active == 0 || (widgetEdit.isOpen() && widgetEditItem == active)) {
            return;
        }
        // a Color / Curve / Gradient window's session is one step for all its widgets (S27 R3)
        if (ui::FieldEditSession::isOpen()) {
            return;
        }
        widgetEdit.commit();
        widgetEdit = history.beginInteractive(std::string());
        widgetEditItem = active;
    };
    // The Edit menu labels and the Undo History panel follow every history change; the loop
    // idles otherwise. An undo or redo of an asset step refreshes the Project window.
    UndoTimeline::onChanged = [this](){
        requestRedraw();
    };
    UndoTimeline::onProjectChanged = [this](){
        if (projectWindow) {
            projectWindow->refreshCurrentDirectory();
        }
    };
    // A physics gesture previews poses in the scene: before any other edit, undo or redo of that
    // scene it settles and commits (or, still held, is cancelled), so nothing records its preview
    CommandHistory::beforeChange = [this](size_t sceneId, CommandHistory::Change change){
        return physicsPlacementWindow->beforeHistoryChange((uint32_t)sceneId, change);
    };
    // the clip editor's history timeline (undo routing, R5b) after the listeners above
    AnimationClipController::installHistoryTimeline(&project);

    ShaderPool::setShaderBuilder([this](jasperengine::ShaderKey shaderKey) -> jasperengine::ShaderBuildResult {
        static jasperengine::editor::ShaderBuilder builder;  // Make static to reuse
        return builder.buildShader(shaderKey, &project);
    }, &jasperengine::editor::ShaderBuilder::getRunningBuildCount);
    // a project UI effect's .frag changed (hot reload, phase 5): its compiled variants go from
    // the builder's cache too, so they compile from the new source (and what draws with a forked
    // shader the registry's reset reached re-binds, as after any shader edit)
    ::jasperengine::htmlui::EffectRegistry::get().setProjectEffectListener([this](const std::string&) {
        project.invalidateCustomShaders();
    });

    Engine::setAsyncLoading(true);

    // Test hook: the harness holds model parses back on their worker, so a save while a load still
    // runs is not left to timing (test_save_determinism.py)
    if (const char* delay = std::getenv("JASPER_TEST_MODEL_LOAD_DELAY_MS")) {
        const long ms = std::strtol(delay, nullptr, 10);
        if (ms > 0) MeshSystem::setModelLoadDelayForTest(static_cast<int>(std::min(ms, 60000L)));
    }

    // Before the project loads, so an MCP client's initialize is answered at once;
    // its tool calls wait in the main-thread queue until the project is ready.
    mcp::McpServer::get().start(*this);
}

void editor::App::engineViewLoaded(){
    reportLoadingProgress("Initializing graphics...");
    Engine::systemViewLoaded();
}

void editor::App::loadStartupProject() {
    lastLoadingFrame = {};
    reportLoadingProgress("Loading project...");
    // the Recorder in every mode (MCP record_game, `call` included)
    setupRecorder();

    // a failed benchmark still runs one frame: the main loop exits on benchmarkExit, and
    // closing before the first frame would leave the render queue waiting on shutdown
    if (benchmarkEnabled) {
        if (benchmarkExit)
            return;

        // absolute already: CommandLine::projectArgPath, while the arguments were parsed
        if (!project.loadProject(benchmarkProject, false)) {
            failBenchmark("failed to load project '" + benchmarkProject.string() + "'");
            return;
        }

        if (!selectBenchmarkScene()) {
            failBenchmark(benchmarkScene.empty() ? "the project has no start scene" : "scene '" + benchmarkScene + "' is not in the project");
            return;
        }

        project.setVSyncEnabled(false);
        if (SceneProject* selected = project.getScene(benchmarkSceneId)) {
            selected->displaySettings.showGrid3D = false;
            selected->displaySettings.showOrigin = false;
            selected->displaySettings.hideSelectionOutline = true;
        }
        projectLoading = false;
        requestRedraw();
        return;
    }

    // S66: what an editor killed during an import or a template copy left, whatever opens now
    hub::staging::setJournalDirectory(AppSettings::getConfigDirectory() / "hub-staging");
    projectHub.startSweep(false);

    const mcp::McpLaunchOptions& mcpOptions = mcp::mcpLaunchOptions();
    if (!mcpOptions.project.empty() || !mcpOptions.createDir.empty()) {
        // `jasperengine-editor mcp --project/--create`: no fallback to the last project
        project.setIgnoreProjectLock(mcpOptions.ignoreLock);
        if (!loadMcpStartupProject(mcpOptions)) {
            if (project.wasProjectLocked()) {
                // another editor writes this project: an agent here would overwrite its work
                std::fprintf(stderr, "[mcp] project_locked: %s Pass --ignore-lock to open it anyway.\n",
                             project.getProjectLockMessage().c_str());
                std::fprintf(stderr, "[mcp] project_locked: closing the editor\n");
                startupExitCode = 3;
                // the main loop exits after the next frame (consumeBenchmarkExit): closing
                // before the first frame leaves the render queue waiting on shutdown. That frame
                // is the loading window (projectLoading stays set, as for a failed benchmark): a
                // refused editor opens nothing, and the temporary project is one directory
                // shared by every editor, so other refused editors may hold its lock
                benchmarkExit = true;
                return;
            }
            if (!project.createTempProject("MyJasperEngineProject")) {
                std::fprintf(stderr, "[mcp] the temporary project could not be opened either: no project is open\n");
            }
        }
        projectLoading = false;
        requestRedraw();
        return;
    }

    setupProjectHub();
    // --project <dir>: that project, never the hub (a failed load keeps the old fallback)
    const hub::LaunchOptions& launch = hub::launchOptions();
    if (!launch.project.empty()) {
        if (!project.loadProject(launch.project)) {
            project.createTempProject("MyJasperEngineProject");
        }
        projectLoading = false;
        requestRedraw();
        return;
    }

    // S27 R16: the Project Hub when Shift is held, --hub, the "Always" preference, or the last
    // project is missing or does not load; otherwise the last project as before
    bool showHub = startupShowsHub();
    std::filesystem::path lastProjectPath = AppSettings::getLastProjectPath();
    if (!showHub) {
        if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath) && project.loadProject(lastProjectPath)) {
            Out::info("Loaded last opened project: \"%s\"", lastProjectPath.string().c_str());
        } else {
            if (!lastProjectPath.empty()) Out::warning("The last project \"%s\" is missing or did not load", lastProjectPath.string().c_str());
            showHub = true;
        }
    }
    if (showHub) {
        // the editor's own temporary project stays behind the hub: never shown, never saved
        project.createTempProject("MyJasperEngineProject");
        hubOverTemp = true;
        projectHub.open(ProjectHubWindow::Page::Projects);
    }

    projectLoading = false;
    requestRedraw();
}

bool editor::App::startupShowsHub() {
    return hub::launchOptions().forceHub || platform::shiftHeld() || AppSettings::getProjectHubAlways();
}

bool editor::App::isRecording() const {
    return recorder::RecorderSession::get().recording();
}

void editor::App::toggleRecording() {
    recorderWindow.toggleRecording();
}

void editor::App::playStarting(uint32_t) {
    recorder::RecorderSession::get().onPlayStarting(recorderWindow.settings());
}

void editor::App::setupRecorder() {
    recorder::Host host;
    auto playScene = [this]() -> SceneProject* { return project.getScene(project.getPlaySessionSceneId()); };
    host.isPlaying = [playScene]() {
        const SceneProject* scene = playScene();
        return scene && (scene->playState == ScenePlayState::PLAYING || scene->playState == ScenePlayState::PAUSED);
    };
    host.isPlayStarting = [playScene]() {
        const SceneProject* scene = playScene();
        return scene && scene->playState == ScenePlayState::LOADING;
    };
    host.startPlay = [this](std::string& error) {
        if (!mainToolbar->canTogglePlay() || mainToolbar->isInPlayMode()) {
            error = "Play cannot start now";
            return false;
        }
        mainToolbar->togglePlay();
        return project.getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    };
    host.stopPlay = [this]() { mainToolbar->stopSession(); };
    host.gameFramebuffer = [this]() { return gameWindow->getFramebuffer(); };
    host.gameScene = [playScene]() -> Scene* {
        const SceneProject* scene = playScene();
        return scene ? scene->scene : nullptr;
    };
    host.setGameRenderSize = [this](int w, int h) { gameWindow->setRenderSizeOverride(w, h); };
    host.gameViewSize = [this](int& w, int& h) {
        w = gameWindow->getWidth();
        h = gameWindow->getHeight();
    };
    host.sceneName = [this, playScene]() {
        const SceneProject* scene = playScene();
        if (!scene) scene = project.getSelectedScene();
        return scene ? scene->name : std::string("Scene");
    };
    host.projectPath = [this]() { return project.getProjectPath(); };
    host.maxTextureSize = []() { return sg_query_limits().max_image_size_2d; };
    recorder::RecorderSession::get().setHost(std::move(host));
    recorderWindow.openPreferences = [this]() { editorSettingsWindow.open(&project, "External Tools"); };
}

void editor::App::setupProjectHub() {
    ProjectHubWindow::Actions actions;
    actions.openProject = [this](const std::filesystem::path& dir, bool withoutLastScene) { hubOpenProject(dir, withoutLastScene); };
    actions.createProject = [this](const hub::ProjectTemplate& info, const std::string& name, const std::filesystem::path& location) {
        hubCreateProject(info, name, location / hub::folderName(name));
    };
    actions.hasProject = [this]() { return !hubOverTemp; };
    projectHub.setActions(std::move(actions));
}

std::string editor::App::projectUnavailableReason() const {
    if (projectHub.isOpen() && hubOverTemp) {
        return "No project is open (the Project Hub is showing): use create_project, open_project or import_project first.";
    }
    return std::string();
}

void editor::App::openProjectHub(ProjectHubWindow::Page page) {
    projectHub.open(page);
    requestRedraw();
}

void editor::App::hubOpenProject(const std::filesystem::path& dir, bool withoutLastScene) {
    if (project.isAnyScenePlaying() || projectLoading || pendingProjectChange) return;
    auto open = [this, dir, withoutLastScene]() {
        requestProjectChange([this, dir, withoutLastScene]() {
            project.setSkipWorkspaceTabs(withoutLastScene);
            if (project.loadProject(dir)) {
                projectHub.close();
                hubOverTemp = false;
            } else {
                project.setSkipWorkspaceTabs(false);
                // the hub stays, over the editor's temporary project again
                project.createTempProject("MyJasperEngineProject");
                hubOverTemp = true;
                hubMessage = "Could not open " + dir.string() + ": see the Console.";
            }
        });
    };
    if (!hubOverTemp && (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges())) {
        registerConfirmAlert("Unsaved Changes", "There are unsaved changes. Do you want to save them before opening another project?",
                             [this, open]() { saveAllAndProject(open); }, open);
    } else {
        open();
    }
}

bool editor::App::createProjectFromTemplate(const std::string& templateId, const std::string& name, const std::filesystem::path& dir,
                                            std::string& error) {
    const std::vector<hub::ProjectTemplate> list = hub::templates::all(hub::templates::userDirectory(AppSettings::getConfigDirectory()));
    const hub::ProjectTemplate* info = hub::templates::find(list, templateId);
    if (!info) {
        std::string known;
        for (const hub::ProjectTemplate& t : list) known += (known.empty() ? "" : ", ") + t.id;
        error = "no template \"" + templateId + "\" (templates: " + known + ")";
        return false;
    }
    const std::string problem = hub::targetProblem(dir);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    if (isCreatingFromTemplate() || projectLoading || pendingProjectChange) {
        error = "a project is being created or opened";
        return false;
    }
    hubCreateProject(*info, name.empty() ? dir.filename().string() : name, dir);
    // the caller (MCP create_project) settled the unsaved changes already
    hubCreationPrompt = false;
    return true;
}

void editor::App::hubCreateProject(const hub::ProjectTemplate& info, const std::string& name, const std::filesystem::path& dir) {
    if (hubCreation || projectLoading || pendingProjectChange) return;
    hubCreationError.clear();
    hubCreationPrompt = true;
    auto creation = std::make_unique<HubCreation>();
    creation->info = info;
    creation->name = name;
    creation->dir = dir;
    const hub::ProjectTemplate copyOf = info;
    const std::filesystem::path target = dir;
    // the files are copied off the main thread; the project then loads on it
    creation->copy = std::async(std::launch::async, [copyOf, target]() {
        std::string error;
        if (!hub::templates::materialize(copyOf, target, error)) return error.empty() ? std::string("the copy failed") : error;
        // a template with no audio node of its own starts like any new project
        if (!AudioMixerDocument::writeNewProjectAudio(target, &error)) return error;
        return std::string();
    });
    hubCreation = std::move(creation);
    requestRedraw();
}

void editor::App::updateHubCreation() {
    if (!hubCreation || hubCreation->copy.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (hubCreation) requestRedraw();
        return;
    }
    const std::string error = hubCreation->copy.get();
    struct Done {
        hub::ProjectTemplate info;
        std::string name;
        std::filesystem::path dir;
    };
    const Done done{hubCreation->info, hubCreation->name, hubCreation->dir};
    const bool prompt = hubCreationPrompt;
    hubCreation.reset();
    if (!error.empty()) {
        hubCreationError = error;
        hubMessage = "Could not create the project: " + error;
        return;
    }
    hubCreationOpening = true;
    auto open = [this, done]() {
        requestProjectChange([this, done]() {
            hubCreationOpening = false;
            if (!project.loadProject(done.dir)) {
                hubCreationError = "the new project did not load";
                std::error_code ec;
                std::filesystem::remove_all(done.dir, ec);
                project.createTempProject("MyJasperEngineProject");
                hubOverTemp = true;
                hubMessage = "Could not open the new project: see the Console.";
                return;
            }
            // the template's project, renamed, saved, with the VCS files (R18; loadProject copied
            // the engine API in)
            project.setName(done.name);
            VcsProjectSettings vcs = project.getVcsProjectSettings();
            vcs.lfs = true;   // S29 R21: a new project's binaries are LFS objects
            project.setVcsProjectSettings(vcs, false);
            project.saveProject();
            project.writeVersionControlMetadata();
            const std::filesystem::path start = done.dir / (done.info.startScene + ".scene");
            std::error_code ec;
            if (!done.info.startScene.empty() && std::filesystem::exists(start, ec)) project.openScene(start);
            AppSettings::addToRecentProjects(done.dir, true, done.name);
            projectHub.close();
            hubOverTemp = false;
        });
    };
    if (prompt && !hubOverTemp && (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges())) {
        registerConfirmAlert("Unsaved Changes", "There are unsaved changes. Do you want to save them before opening the new project?",
                             [this, open]() { saveAllAndProject(open); }, open);
    } else {
        open();
    }
}

void editor::App::writeHubThumbnail() {
    if (project.getProjectPath().empty() || project.isEditorTempProject()) return;
    SceneProject* scene = project.getSelectedScene();
    if (!scene || !scene->sceneRender) return;
    std::vector<uint8_t> png;
    if (!GraphicUtils::encodeFramebufferThumbnailPng(scene->sceneRender->getFramebuffer(), 320, 180, png) || png.empty()) return;
    const std::filesystem::path file = project.getProjectInternalPath() / "user" / "hub-thumbnail.png";
    const io::WriteResult written = io::writeFileAtomic(file, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    if (!written.ok()) Out::warning("Project Hub thumbnail: %s", written.message.c_str());
}

void editor::App::drawSaveProjectAsTemplate() {
    if (saveTemplatePopup) {
        ImGui::OpenPopup("Save Project as Template##saveTemplate");
        saveTemplatePopup = false;
    }
    if (!EditorModal::begin("Save Project as Template##saveTemplate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(Theme::dpi(320.0f));
    EditorModal::inputText("##templateName", "name", saveTemplateName.data(), saveTemplateName.size());
    ImGui::TextUnformatted("Description");
    EditorModal::field("description", saveTemplateDescription.data(), saveTemplateDescription.size());
    ImGui::InputTextMultiline("##templateDescription", saveTemplateDescription.data(), saveTemplateDescription.size(),
                              ImVec2(Theme::dpi(320.0f), Theme::dpi(70.0f)));
    const std::string id = hub::slugify(saveTemplateName.data());
    const std::filesystem::path userDir = hub::templates::userDirectory(AppSettings::getConfigDirectory());
    std::error_code ec;
    std::string problem = !hub::isValidCatalogId(id) ? "The name needs letters or digits." : std::string();
    if (problem.empty() && std::filesystem::exists(userDir / id, ec)) problem = "A template \"" + id + "\" exists.";
    EditorModal::message("Save this project as a template of the Project Hub.");
    EditorModal::error(!saveTemplateMessage.empty() ? saveTemplateMessage : problem);
    if (!problem.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", problem.c_str());
    else ImGui::TextDisabled("%s", (userDir / id).string().c_str());
    if (!saveTemplateMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", saveTemplateMessage.c_str());
    ImGui::BeginDisabled(!problem.empty());
    if (EditorModal::button("Save", "ok", ImVec2(Theme::dpi(100.0f), 0))) {
        // the thumbnail is the Scene view (480 wide)
        std::vector<uint8_t> png;
        if (SceneProject* scene = project.getSelectedScene(); scene && scene->sceneRender) {
            GraphicUtils::encodeFramebufferThumbnailPng(scene->sceneRender->getFramebuffer(), 480, 270, png);
        }
        std::string startScene;
        if (SceneProject* scene = project.getSelectedScene(); scene && !scene->filepath.empty()) startScene = scene->filepath.stem().string();
        std::string savedId, error;
        if (hub::templates::saveProjectAsTemplate(project.getProjectPath(), userDir, saveTemplateName.data(), saveTemplateDescription.data(),
                                                  startScene, png, savedId, error)) {
            Out::info("Saved the project as the template \"%s\" (%s)", saveTemplateName.data(), (userDir / savedId).string().c_str());
            ImGui::CloseCurrentPopup();
        } else {
            saveTemplateMessage = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(Theme::dpi(100.0f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool editor::App::loadMcpStartupProject(const mcp::McpLaunchOptions& options) {
    std::string error;
    if (!options.project.empty()) {
        if (project.openProjectNow(options.project, &error)) {
            Out::info("Opened project for the MCP client: \"%s\"", options.project.string().c_str());
            return true;
        }
        if (!project.wasProjectLocked()) {
            std::fprintf(stderr, "[mcp] could not open the project at %s: %s; an empty temporary project is open instead\n",
                         options.project.string().c_str(), error.c_str());
        }
        return false;
    }

    // --create: File > New Project + Save Project without the dialogs
    if (project.createProjectAt(options.createDir, options.createName, &error)) {
        std::fprintf(stderr, "[mcp] created project %s at %s\n", project.getName().c_str(), options.createDir.string().c_str());
        return true;
    }
    std::fprintf(stderr, "[mcp] could not create the project at %s: %s; an empty temporary project is open instead\n",
                 options.createDir.string().c_str(), error.c_str());
    return false;
}

void editor::App::quitWithoutPrompts(const std::string& reason) {
    // Nothing is saved for the agent: report what is left unsaved instead
    std::vector<std::string> unsaved;
    for (const SceneProject& sceneProject : project.getScenes()) {
        if (sceneProject.isModified) unsaved.push_back("scene \"" + sceneProject.name + "\"");
        else if (sceneProject.filepath.empty()) unsaved.push_back("scene \"" + sceneProject.name + "\" (never saved)");
    }
    if (codeEditor->hasUnsavedChanges()) unsaved.push_back("code editor buffers");
    if (project.isTempUnsavedProject()) unsaved.push_back("the temporary project itself");
    if (!unsaved.empty()) {
        std::string list;
        for (const std::string& item : unsaved) list += (list.empty() ? "" : ", ") + item;
        std::fprintf(stderr, "[mcp] %s: closing with UNSAVED changes (not saved): %s\n", reason.c_str(), list.c_str());
        Out::warning("%s: closing with unsaved changes: %s", reason.c_str(), list.c_str());
    } else {
        std::fprintf(stderr, "[mcp] %s: closing the editor\n", reason.c_str());
    }
    if (projectLoading) {
        Backend::closeWindow();
        return;
    }
    closeWindow();
}

bool editor::App::getOpenDialog(EditorDialogInfo& out) const {
    out = EditorDialogInfo();
    // it holds the main thread, so nothing below can change while it is open
    if (EditorFilePicker::info(out)) return true;
    if (alert.needShow) {
        out.kind = "alert";
        out.title = alert.title;
        out.message = alert.message;
        if (!alert.note.empty()) out.message += " (" + alert.note + ")";
        switch (alert.type) {
            case AlertType::Info:
                out.choices = {"ok"};
                break;
            case AlertType::Confirm:
                out.choices = {"yes", "no"};
                break;
            case AlertType::ThreeButton:
                out.choices = {"yes", "no", "cancel"};
                out.message += " [yes = " + alert.yesLabel + ", no = " + alert.noLabel + ", cancel = Cancel]";
                break;
        }
        return true;
    }
    // Every other modal, as the last frame drew it (MCP calls run before this frame's windows)
    const bool modal = EditorModal::topModal(out);
    if (!sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) return modal;
    const std::string saveTitle = sceneSaveDialog.isOpen() ? "Save Scene" : "Save Project";
    // something it opened, on top of it
    if (modal && out.title != saveTitle) return true;
    out.kind = "save_dialog";
    out.title = saveTitle;
    // a save dialog no frame has drawn yet has only its cancel, which needs no frame
    if (!modal) {
        out.message = "asks where to save; cancel it and save with an explicit path instead";
        out.choices = {"cancel"};
    }
    return true;
}

bool editor::App::answerDialog(const std::string& choice, const std::map<std::string, EditorDialogValue>& values, std::string& error) {
    EditorDialogInfo dialog;
    if (!getOpenDialog(dialog)) {
        error = "No dialog is open.";
        return false;
    }
    if (dialog.kind == "alert") {
        if (!values.empty()) {
            error = "The alert \"" + dialog.title + "\" has no fields to fill.";
            return false;
        }
        if (std::find(dialog.choices.begin(), dialog.choices.end(), choice) == dialog.choices.end()) {
            error = "Choice must be one of:";
            for (const std::string& valid : dialog.choices) error += " " + valid;
            error += ".";
            return false;
        }
        // Pressed in showAlert on the next frame, through the button's own code path
        alertPendingChoice = choice;
    } else if (dialog.kind == "save_dialog" && choice == "cancel") {
        sceneSaveDialog.requestCancel();
        projectSaveDialog.requestCancel();
    } else if (dialog.choices.empty() && dialog.disabledChoices.empty() && dialog.fields.empty()) {
        error = "The dialog \"" + dialog.title + "\" can only be closed by a human in the editor window.";
        return false;
    } else if (!EditorModal::answer(choice, values, error)) {
        // pressed when the modal is next drawn, through the button's own code path
        return false;
    }
    requestRedraw();
    return true;
}

void editor::App::requestProjectChange(std::function<void()> change) {
    if (projectLoading || pendingProjectChange) return;
    // the Sprite Normal Map window's unsaved paint strokes are asked about first
    if (spriteNormalsWindow && spriteNormalsWindow->hasUnsavedPaint()) {
        registerConfirmAlert("Discard Paint Changes?",
                             "The Sprite Normal Map window has paint strokes that are not saved into the normal map.\n\nDiscard them and switch projects?",
                             [this, change]() {
                                 spriteNormalsWindow->discardPaint();
                                 requestProjectChange(change);
                             });
        return;
    }
    pendingProjectChange = std::move(change);
    requestRedraw();
}

void editor::App::beforeFrame() {
    processProjectChange();
    // a layout applies between frames: a preset picked, or a project that opened (S25 R17)
    layouts.beforeFrame();
}

void editor::App::processProjectChange() {
    if (projectLoading || !pendingProjectChange) return;
    // a scene save in flight finishes into the project it started in
    if (project.isAnySceneSaving()) return;
    auto change = std::move(pendingProjectChange);
    pendingProjectChange = nullptr;
    if (project.isAnyScenePlaying()) return;
    // the old project's save dialogs never act on the next one
    cancelSaveDialogs();

    // Menu and dialog callbacks queue the change. Run it between frames so the
    // loading pump can draw without nesting ImGui frames or invalidating scene UI.
    projectLoading = true;
    lastLoadingFrame = {};
    Backend::setMainMenu({}, {});
    try {
        reportLoadingProgress("Loading project...");
        change();
    } catch (const std::exception& e) {
        Out::error("Failed to switch projects: %s", e.what());
        registerAlert("Error", "Failed to switch projects.");
    }
    projectLoading = false;
    requestRedraw();
}

// Every loop iteration, idle ones too (engineRender only runs on drawn frames): the file
// watcher's changes and the reloads they started (S05), HTML UI imports, and the document
// animations due now wake the loop up
void editor::App::pollExternalChanges(){
    HotReloadService& reloads = HotReloadService::get();
    // an MCP call that may write files holds the watcher's batches: its own sync reloads
    // them once (R30)
    reloads.setPaused(mcp::McpAutoSync::get().writingCallInFlight());
    bool wake = FileWatchService::get().pump(PUMP_BUDGET_MS);
    wake = reloads.update() || wake;
    wake = ScriptStatus::get().update() || wake;
    wake = ScriptReloadService::get().busy() || wake;
    wake = htmlui::HtmlUiAutoImport::get().update(&project) || wake;
    // the asset database's walk a watcher batch started (S04 refresh, no poll)
    project.updateAssetScan();
    // texture cooks (S14 R8): planned and cooked on the Background lane, swapped in here
    if (!projectLoading && !project.getProjectPath().empty()) {
        TextureCookQueue& cooks = TextureCookQueue::get();
        if (cooks.getProject() != &project) cooks.attach(&project);
        wake = cooks.update() || wake;
        // and model cooks (S53 R13): cooked on the Background lane, the models showing them reloaded here
        ModelCookQueue& models = ModelCookQueue::get();
        if (models.getProject() != &project) models.attach(&project);
        wake = models.update() || wake;
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& pair : htmlUIRedrawTimes) {
        if (now >= pair.second) wake = true;
    }

    if (wake) requestRedraw();
}

bool editor::App::wantsHtmlUIRedraw(const SceneProject& sceneProject){
    UIDocumentSystem* documents = sceneProject.scene->getSystem<UIDocumentSystem>().get();
    const double delay = documents ? documents->getNextUpdateDelay() : std::numeric_limits<double>::infinity();
    const auto now = std::chrono::steady_clock::now();

    if (!std::isfinite(delay)) {
        htmlUIRedrawTimes.erase(sceneProject.id);
        return false;
    }
    if (delay <= 0.0) {
        htmlUIRedrawTimes.erase(sceneProject.id);
        return true;
    }

    // a later update (a transition delay, the next keyframe): wake up then, without
    // redrawing until it is due
    auto it = htmlUIRedrawTimes.find(sceneProject.id);
    if (it == htmlUIRedrawTimes.end()) {
        htmlUIRedrawTimes[sceneProject.id] = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(delay));
        return false;
    }
    if (now >= it->second) {
        htmlUIRedrawTimes.erase(it);
        return true;
    }
    return false;
}

void editor::App::applySystemFonts(){
    SystemFonts::applyPending(systemFontTargets);
}

void editor::App::engineRender(){
    if (projectLoading) {
        SystemRender::executeQueue();
        return;
    }

    // S27: the game frame the last editor frame drew, read back before this one updates
    recorder::RecorderSession::get().captureFrame();

    // a frame that began drawing (EditorFrame: frameReady): PoseRender::renderNow may draw
    // from here on, main-thread tasks included, until the end of this function
    struct PoseFrameScope {
        PoseFrameScope() { PoseRender::setInEditorFrame(true); ParticleEffectPreview::setInEditorFrame(true); }
        ~PoseFrameScope() { PoseRender::setInEditorFrame(false); ParticleEffectPreview::setInEditorFrame(false); }
    } poseFrameScope;

    processMainThreadTasks();
    // upstream's FBX reimports: their own throttled check, hashing on a Background job
    // (not the hot reload's watcher yet)
    project.refreshFbxImports();
    // scripts saved while a scene plays swap in here, at the frame boundary (no Lua runs now)
    ScriptStatus::get().update();
    ScriptReloadService::get().update();
    // navigation bakes of the stopped scenes: landed and saved (.navmesh) as they finish
    NavigationEditor::get().update(&project);
    // generated normal maps whose sources changed (badges, the opt-in auto-regenerate)
    spriteNormalsWatcher->tick();
    project.updateGeneratedSources();
    // clip previews (the Animation window's clip mode, MCP preview_animation) advance every frame
    if (animationClipAppHooks().frame) {
        animationClipAppHooks().frame(&project, ImGui::GetIO().DeltaTime, ImGui::GetFrameCount());
    }
    // Prefab Mode's Auto Save waits for the edit in progress (a drag, a field being typed)
    project.updatePrefabStages(ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()
        || ImGui::GetIO().WantTextInput || physicsPlacementWindow->isAnyBusy());
    // scene Auto Save (backups only), never in the middle of a drag, a field being typed or a
    // Physics Placement gesture (its preview poses are in the scene)
    project.tickAutosave(ImGui::GetIO().DeltaTime, ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()
        || ImGui::GetIO().WantTextInput || physicsPlacementWindow->isAnyBusy());
    // the title says when the scene shown did not load completely (its tab does too)
    {
        const SceneProject* shown = project.getSelectedScene();
        const std::string suffix = shown && shown->loadReport.dropped() > 0 ?
            " - " + (shown->filepath.empty() ? shown->name : shown->filepath.filename().string()) + " (Partial Load)" : std::string();
        if (suffix != windowTitleSuffix) {
            windowTitleSuffix = suffix;
            Backend::updateWindowTitle((windowTitleProject.empty() ? project.getName() : windowTitleProject) + windowTitleSuffix);
        }
    }
    renderedSceneThisFrame = false;
    const uint32_t selectedSceneId = project.getSelectedSceneId();

    // Redraw the selected scene only when it changed (needUpdateRender) or while a
    // continuous state (play/load/preview/thumbnail) is active, so the loop can idle.
    const bool thumbnailsPending = projectWindow && projectWindow->hasPendingThumbnailWork();

    // Drop targets preview a dragged resource directly, with no command to flag the scene.
    const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
    const bool draggingResourceFile = dragPayload && dragPayload->IsDataType("resource_files");

    // Keep drawing a few frames after the last change so late async uploads and
    // shader builds still get presented.
    const int RENDER_SETTLE_FRAMES = 30;

    auto sceneNeedsRender = [&](SceneProject& sp, bool isSelected) -> bool {
        bool active = sp.needUpdateRender;
        bool gaugeAnimating = false;
        if (benchmarkEnabled || wantsContinuousFrames()) active = true;
        // A running play session must advance and redraw its simulation every frame.
        // (unless a held simulation, which only draws on demand like a paused one)
        if (sp.playState == ScenePlayState::PLAYING && !Engine::isSimulationHeld()) active = true;
        // A drag preview redraws until the settle frames cover its restore.
        if (draggingResourceFile) active = true;
        if (isSelected && sp.scene && sp.sceneRender) {
            // Still loading a model, previewing a camera, or capturing a thumbnail.
            auto ms = sp.scene->getSystem<MeshSystem>();
            if (ms && (ms->hasPendingAsyncModelLoads() || ms->hasPendingFoliageUpdates())) active = true;
            // A parsed model still has per-mesh work that only runs while drawing.
            auto rs = sp.scene->getSystem<RenderSystem>();
            if (rs && !rs->isAllLoaded()) active = true;
            // isAllLoaded skips empty UIs, and an image builds its geometry only once its texture loads
            auto us = sp.scene->getSystem<UISystem>();
            if (us && us->hasPendingImages()) active = true;
            // HTML documents (and their shaders) loading, until one frame shows them
            if (auto ds = sp.scene->getSystem<UIDocumentSystem>(); ds && !ds->isAllLoaded()) active = true;
            if (sp.sceneRender->isPreviewCameraActive()) active = true;
            gaugeAnimating = sp.sceneRender->getUILayer()->isCameraGaugeAnimating();
            if (thumbnailsPending) active = true;
            // Preview scenes (shape, material, direction) only draw inside systemDraw().
            if (Engine::hasScenesToExecuteOnce()) active = true;
        }

        // A stopped scene's HTML documents running a CSS transition or animation: redraw
        // while RmlUi asks for the next update, at the time it asks for (edit-mode frames
        // advance the UI clock through Engine::systemDraw)
        if (!active && sp.scene && sp.playState != ScenePlayState::PLAYING && wantsHtmlUIRedraw(sp)) active = true;

        if (active) {
            sp.renderSettleFrames = RENDER_SETTLE_FRAMES;
            return true;
        }
        if (gaugeAnimating) {
            if (sp.renderSettleFrames > 0) sp.renderSettleFrames--;
            return true;
        }
        if (sp.renderSettleFrames > 0) {
            sp.renderSettleFrames--;
            return true;
        }
        return false;
    };

    // The Scene views: the open scenes and the shown prefab stage (Prefab Mode)
    std::vector<SceneProject*> viewScenes;
    for (auto& openScene : project.getScenes()) {
        if (openScene.opened) viewScenes.push_back(&openScene);
    }
    if (PrefabStage* prefabStage = project.getActivePrefabStage()) {
        viewScenes.push_back(&prefabStage->sceneProject);
    }

    for (SceneProject* viewScene : viewScenes) {
        SceneProject& sceneProject = *viewScene;
        if (!sceneProject.scene || !sceneProject.sceneRender) continue;
        if (sceneProject.playState == ScenePlayState::SAVING || sceneProject.playState == ScenePlayState::LOADING || sceneProject.playState == ScenePlayState::CANCELLING) continue;

        const bool isSelected = (sceneProject.id == selectedSceneId);

        // The running game renders into the Game view's framebuffer, shown or not, and
        // the Scene view is a second, draw-only view of the same frame
        if (gameWindow->hostsScene(sceneProject.id)) {
            // send_input app hide: the game updates without drawing, its Game view keeps the
            // last frame
            if (Platform::isGameHiddenSimulated() && runningGameScene() == &sceneProject) {
                runHiddenGameFrame();
                continue;
            }
            if (sceneNeedsRender(sceneProject, isSelected)) {
                renderRunningScene(sceneProject);
            }
            continue;
        }

        // Beside a running game the Engine's frame is the game's: another scene's view
        // draws on its own, so the game still updates once per frame
        if (project.getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
            if (sceneProject.isVisible && sceneNeedsRender(sceneProject, isSelected)) {
                renderSceneBesideGame(sceneProject);
            }
            continue;
        }

        // A stopped scene draws its Scene view; the Game view's preview through its game
        // camera is a second view of that frame
        const bool gameViewPass = gameWindow->wantsEditPass(sceneProject.id);

        auto meshSystem = sceneProject.scene->getSystem<MeshSystem>();
        bool hasPendingModelLoads = meshSystem && meshSystem->hasPendingAsyncModelLoads();

        // A tab hidden behind another has nothing to present, and drawing it makes
        // lastActivatedScene alternate between tabs. The selected one always draws,
        // unless the Game view is what shows it.
        const bool hiddenTab = !sceneProject.isVisible
            && (!isSelected || gameViewPass)
            && sceneProject.playState != ScenePlayState::PLAYING;
        const bool drawSceneView = !hiddenTab && !(hasPendingModelLoads && !sceneProject.needUpdateRender && !isSelected);
        if (!drawSceneView && !gameViewPass) {
            if (hasPendingModelLoads) meshSystem->update(0);
            continue;
        }

        const bool needsRender = sceneNeedsRender(sceneProject, isSelected);
        const bool gameViewPassDue = gameViewPass && (needsRender || gameWindow->isEditPassDirty());

        int width = sceneWindow->getWidth(sceneProject.id);
        int height = sceneWindow->getHeight(sceneProject.id);

        // Not laid out yet (background tab): nothing to draw, and activating it here
        // would thrash lastActivatedScene. Stays dirty until SceneWindow gives it a size.
        // A due Game view preview draws the scene too, since it is a view of that frame.
        bool sceneViewDrawn = false;
        if (drawSceneView && (needsRender || gameViewPassDue) && width != 0 && height != 0){
            SceneRender* sceneRender = sceneProject.sceneRender;

            bool sceneChanged = false;

            // Collect loaded child scene layers (skip if playing — runtime manages its own layers)
            std::vector<Scene*> childLayers;
            if (sceneProject.playState == ScenePlayState::STOPPED) {
                for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                    uint32_t childId = childSceneRef.id;
                    const SceneProject* childScene = project.getScene(childId);
                    if (childScene && childScene->expandedInline && childScene->scene) {
                        childLayers.push_back(childScene->scene);
                    }
                }
            }
            sceneProject.sceneRender->setChildSceneLayers(childLayers);
            // Prefab Mode In Context: the context scene under the stage (no-op for a scene)
            project.preparePrefabStageView(sceneProject.id, width, height);

            // activate() reloads this scene and every layer, so it runs only on a scene
            // change. Anything else needing it calls resetLastActivatedScene().
            if (lastActivatedScene != sceneProject.id || lastActivatedGame){
                // Collected before activate(): it drops every engine layer, and a dropped
                // layer no longer counts as running, so this would come back empty.
                std::vector<Scene*> runtimeLayers = project.getRunningRuntimeLayers(sceneProject.id);
                sceneProject.sceneRender->activate();

                for (Scene* runtimeLayer : runtimeLayers) {
                    Engine::addSceneLayer(runtimeLayer);
                }

                lastActivatedScene = sceneProject.id;
                lastActivatedGame = false;
                pendingResizeScene = sceneProject.id;
                sceneChanged = true;
                #ifdef _DEBUG
                printf("DEBUG: Activated scene %u\n", lastActivatedScene);
                #endif
            }

            if (pendingResizeScene == sceneProject.id) {
                sceneChanged = true;
                pendingResizeScene = NULL_PROJECT_SCENE;
            }

            sceneRender->setOverlayScale(sceneWindow->getOverlayScale(sceneProject.id));
            Framebuffer* framebuffer = Engine::getFramebuffer();
            const bool framebufferMismatch = !framebuffer
                || !framebuffer->isCreated()
                || framebuffer->getWidth() != static_cast<unsigned int>(width)
                || framebuffer->getHeight() != static_cast<unsigned int>(height);

            if (Platform::setSizes(width, height) || sceneChanged || framebufferMismatch){
                Engine::systemViewChanged();
                sceneRender->updateSize(width, height);
                sceneChanged = false;
            }

            // Update child scene render systems so their entities display correctly
            for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                uint32_t childId = childSceneRef.id;
                SceneProject* childScene = project.getScene(childId);
                if (childScene && childScene->expandedInline && childScene->scene) {
                    if (childScene->sceneRender) {
                        childScene->sceneRender->hideAllGizmos();
                    }
                    childScene->scene->getSystem<MeshSystem>()->update(0);
                    childScene->scene->getSystem<UISystem>()->update(0);
                    childScene->scene->getSystem<UIDocumentSystem>()->update(0);
                    childScene->scene->getSystem<RenderSystem>()->update(0);
                    childScene->needUpdateRender = false;
                }
            }

            // to avoid delay when move objects with gizmo
            sceneRender->updateRenderSystem();

            project.updateSceneRender(sceneProject.id);

            // A paused game advances one frame here when Step was pressed
            const bool steppingFrame = project.beginStepFrame(sceneProject.id);

            project.beginPrefabStageDraw(sceneProject.id);
            Engine::systemDraw();
            // Split and Quad: the other panes, after the frame (only those that changed)
            sceneWindow->renderInactivePanes(sceneProject);
            project.endPrefabStageDraw(sceneProject.id);

            if (steppingFrame) {
                project.endStepFrame(sceneProject.id);
            }

            projectWindow->processMaterialThumbnails();
            projectWindow->processModelThumbnails();
            projectWindow->processPrefabThumbnails();

            // Only a real draw counts as activity, or the loop never idles.
            renderedSceneThisFrame = true;
            sceneProject.needUpdateRender = false;
            sceneViewDrawn = true;
        }

        if (gameViewPassDue) {
            if (sceneViewDrawn) {
                renderGameViewSecondary(sceneProject);
            } else {
                // no Scene view frame to share (its tab is hidden): the preview is the frame
                renderGameViewPass(sceneProject);
                // (and what it drew for the Project window is taken here)
                projectWindow->processMaterialThumbnails();
                projectWindow->processModelThumbnails();
                projectWindow->processPrefabThumbnails();
            }
        }
    }

    // A frame resuming after an idle gap measures the gap, not a frame time.
    if (renderedSceneThisFrame && renderedScenePrevFrame){
        footerFramerate = Engine::getFramerate();
        footerDeltaMs = (float)(Engine::getDeltatime() * 1000.0);
    }else{
        footerFramerate = 0.0f;
        footerDeltaMs = 0.0f;
    }
    renderedScenePrevFrame = renderedSceneThisFrame;

    // offscreen pose renders (PoseRender): private scenes drawn as views of their own after
    // this frame's views, so the open scenes, the Scene view and the Game view are untouched
    if (PoseRender::processFrame()) {
        requestRedraw();
    }
    // the Vertex Animation Baker's preview views (private scenes too, drawn the same way)
    VatPreviewView::processFrame();
    // particle previews and filmstrips (preview_particle_effect, the Particle Effect window): their
    // private scenes step and draw here, whether a view shows or not
    ParticleEffectPreview::processFrame();
    // Procedural Material evaluations and their lit balls (preview_procedural_material, bake_procedural_material):
    // the evaluator draws its own passes here, outside any render pass
    procmat::ProceduralRun::processFrame();

    if (benchmarkEnabled)
        tickBenchmark();
}

// The running game: drawn through the Engine at the Game view's size into its
// framebuffer (whether the Game tab is shown or not), then, when the Scene view is
// shown, the same frame again from the editor camera with the gizmos (Unity's Scene
// view while playing). The game's systems update once, in the Engine's frame.
void editor::App::renderRunningScene(SceneProject& sceneProject) {
    SceneRender* sceneRender = sceneProject.sceneRender;
    Framebuffer* gameTarget = gameWindow->getFramebuffer();

    const int gameWidth = gameWindow->getRenderWidth();
    const int gameHeight = gameWindow->getRenderHeight();

    const int sceneWidth = sceneWindow->getWidth(sceneProject.id);
    const int sceneHeight = sceneWindow->getHeight(sceneProject.id);
    const bool sceneViewShown = sceneProject.isVisible && sceneWidth > 0 && sceneHeight > 0;

    // the runtime manages its own layers while it plays
    sceneRender->setChildSceneLayers({});
    // the frame is the game: it runs at the Game view's Preview Atmosphere Quality
    if (sceneProject.scene)
        sceneProject.scene->getSystem<RenderSystem>()->setPreviewAtmosphereQuality(gameWindow->getSettings().atmosphereQuality);
    // and with the Game view's simulated Device Tier, platform family and thermal state (Play
    // stop ends it: Engine::resetApplicationState)
    Device::setSimulation(gameWindow->getDeviceSimulation());

    bool viewChanged = activateRunningGame(sceneProject);

    if (pendingResizeScene == sceneProject.id) {
        viewChanged = true;
        pendingResizeScene = NULL_PROJECT_SCENE;
    }

    const bool framebufferMismatch = !gameTarget->isCreated()
        || gameTarget->getWidth() != static_cast<unsigned int>(gameWidth)
        || gameTarget->getHeight() != static_cast<unsigned int>(gameHeight);

    // the Engine's view is the game's: the Scene view never changes it. A simulated device's
    // orientation goes with the size it was laid out for (Screen.orientation, onOrientationChanged).
    Platform::setShownOrientation(gameWindow->getLayoutOrientation());
    if (Platform::setSizes(gameWidth, gameHeight) || viewChanged || framebufferMismatch){
        Engine::systemViewChanged();
    }else{
        // a half turn keeps the size
        Engine::systemOrientationChanged();
    }

    // the Scene view's camera follows its size before the frame updates the scene
    if (sceneViewShown){
        sceneRender->setOverlayScale(sceneWindow->getOverlayScale(sceneProject.id));
        if (sceneRender->getViewWidth() != sceneWidth || sceneRender->getViewHeight() != sceneHeight){
            sceneRender->updateSize(sceneWidth, sceneHeight);
        }
    }

    // to avoid delay when move objects with gizmo
    sceneRender->updateRenderSystem();

    // A paused game advances one frame here when Step was pressed
    const bool steppingFrame = project.beginStepFrame(sceneProject.id);

    Engine::systemDraw();

    if (steppingFrame) {
        project.endStepFrame(sceneProject.id);
    }

    // the gizmos follow the objects where this frame's update left them
    if (sceneViewShown){
        project.updateSceneRender(sceneProject.id);
        // after a game load replaced the played scene, the Scene view shows the scene the game
        // runs now, through a copy of the editor camera placed in it
        Scene* replacingBase = nullptr;
        Entity replacingCamera = NULL_ENTITY;
        if (SceneProject* loaded = project.getReplacingMainScene(sceneProject.id); loaded && loaded->scene && sceneRender->getCamera()) {
            replacingCamera = ViewCameraMirror::ensure(loaded->scene, loaded->sceneViewMirror, "Scene View Camera");
            if (ViewCameraMirror::copy(sceneProject.scene, sceneRender->getCamera()->getEntity(), loaded->scene, replacingCamera)) {
                loaded->scene->getSystem<RenderSystem>()->updateTransform(loaded->scene->getComponent<Transform>(replacingCamera), replacingCamera);
                replacingBase = loaded->scene;
            }
        }
        sceneRender->drawSceneView(sceneWidth, sceneHeight, project.getRunningRuntimeLayers(sceneProject.id), true,
                                   replacingBase, replacingBase ? replacingCamera : NULL_ENTITY);
        sceneWindow->renderInactivePanes(sceneProject);
    }

    projectWindow->processMaterialThumbnails();
    projectWindow->processModelThumbnails();
    projectWindow->processPrefabThumbnails();

    renderedSceneThisFrame = true;
    sceneProject.needUpdateRender = false;
}

bool editor::App::activateRunningGame(SceneProject& sceneProject) {
    if (lastActivatedScene == sceneProject.id && lastActivatedGame)
        return false;

    // Collected before activateGame(): it drops every engine layer, and a dropped
    // layer no longer counts as running, so this would come back empty.
    std::vector<Scene*> runtimeLayers = project.getRunningRuntimeLayers(sceneProject.id);
    sceneProject.sceneRender->activateGame(gameWindow->getFramebuffer());

    if (project.isPlayedSceneReplaced(sceneProject.id)) {
        // a scene load replaced the played scene (or rebuilt it, Restart): the game is the
        // stack that load made, its main scene first
        Engine::removeScene(sceneProject.scene);
        Scene* loadedMain = SceneManager::getScenePtr(SceneManager::getCurrentSceneId());
        if (loadedMain && std::find(runtimeLayers.begin(), runtimeLayers.end(), loadedMain) != runtimeLayers.end()) {
            Engine::setScene(loadedMain);
        }
    }
    for (Scene* runtimeLayer : runtimeLayers) {
        Engine::addSceneLayer(runtimeLayer);
    }

    lastActivatedScene = sceneProject.id;
    lastActivatedGame = true;
    return true;
}

const editor::SceneProject* editor::App::runningGameScene() const {
    if (projectLoading || Engine::isSimulationHeld())
        return nullptr;
    const SceneProject* sceneProject = project.getScene(project.getPlaySessionSceneId());
    if (!sceneProject || sceneProject->playState != ScenePlayState::PLAYING || !sceneProject->scene || !sceneProject->sceneRender)
        return nullptr;
    return sceneProject;
}

bool editor::App::isHiddenGameRunning() const {
    return (!windowVisible || Platform::isGameHiddenSimulated()) && runningGameScene();
}

// An exported game minimized keeps running without drawing (Engine::getHiddenUpdateFps), and so
// does Play while the editor window can't be seen or the game is hidden by send_input. Only the
// game's own frame: no Scene view, no Step, no Recorder capture, no view size change.
bool editor::App::runHiddenGameFrame() {
    const SceneProject* running = runningGameScene();
    if (!running) {
        hiddenGameClock.reset();
        return false;
    }
    SceneProject* sceneProject = project.getScene(running->id);
    // a wake-up a millisecond early counts (the loop sleeps until this deadline)
    if (!hiddenGameClock.isDue(Engine::getHiddenUpdateFps(), 0.001))
        return false;

    // scripts saved meanwhile swap in at the frame boundary, as in engineRender
    ScriptStatus::get().update();
    ScriptReloadService::get().update();
    if (activateRunningGame(*sceneProject)) {
        // the view of the stack activated here is set when the game draws again
        pendingResizeScene = sceneProject->id;
    }
    // waits for the GPU on Metal first
    Engine::systemUpdate();
    return true;
}

// A stopped scene's Scene view while another scene plays: its systems update here (the
// Engine's frame is the game's) and the view draws on its own, with its inline child
// scenes and the editor overlays
void editor::App::renderSceneBesideGame(SceneProject& sceneProject) {
    const int width = sceneWindow->getWidth(sceneProject.id);
    const int height = sceneWindow->getHeight(sceneProject.id);
    if (width <= 0 || height <= 0) {
        return;
    }

    SceneRender* sceneRender = sceneProject.sceneRender;

    std::vector<Scene*> childLayers;
    for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
        SceneProject* childScene = project.getScene(childSceneRef.id);
        if (childScene && childScene->expandedInline && childScene->scene) {
            childLayers.push_back(childScene->scene);
            if (childScene->sceneRender) {
                childScene->sceneRender->hideAllGizmos();
            }
            // nothing activates a view beside the game: its camera comes from here
            Project::prepareInlineChildForView(childScene->scene);
            childScene->scene->update(Engine::getDeltatime());
            childScene->needUpdateRender = false;
        }
    }
    sceneRender->setChildSceneLayers(childLayers);
    project.preparePrefabStageView(sceneProject.id, width, height);

    sceneRender->setOverlayScale(sceneWindow->getOverlayScale(sceneProject.id));
    if (sceneRender->getViewWidth() != width || sceneRender->getViewHeight() != height) {
        sceneRender->updateSize(width, height);
    }
    project.updateSceneRender(sceneProject.id);
    sceneRender->updateRenderSystem();
    sceneProject.scene->update(Engine::getDeltatime());

    project.beginPrefabStageDraw(sceneProject.id);
    sceneRender->drawSceneView(width, height, childLayers, false);
    sceneWindow->renderInactivePanes(sceneProject);
    project.endPrefabStageDraw(sceneProject.id);

    renderedSceneThisFrame = true;
    sceneProject.needUpdateRender = false;
}

// The Game view's edit-mode preview as a second view of the Scene view's frame: the
// scene and the child scenes the game starts with, through their game cameras, framed
// like the game frames the project canvas.
void editor::App::renderGameViewSecondary(SceneProject& sceneProject) {
    const int width = gameWindow->getWidth();
    const int height = gameWindow->getHeight();
    const Entity cameraEntity = project.getGameCamera(sceneProject.id);
    if (width <= 0 || height <= 0 || cameraEntity == NULL_ENTITY) {
        return;
    }

    Framebuffer* target = gameWindow->getFramebuffer();
    if (!target->isCreated() || target->getWidth() != (unsigned int)width || target->getHeight() != (unsigned int)height) {
        target->destroy();
        target->setWidth((unsigned int)width);
        target->setHeight((unsigned int)height);
        JE_GPU_ALLOC_SCOPE(RenderTargets, "Game view");
        target->create();
    }

    int canvasWidth = 0;
    int canvasHeight = 0;
    Rect viewRect;
    Engine::computeViewLayout(width, height, (int)project.getCanvasWidth(), (int)project.getCanvasHeight(), project.getScalingMode(),
                              canvasWidth, canvasHeight, viewRect);

    // a UI scene's camera covers its Canvas Scaler UI size (what it is laid out at), any other the canvas
    auto layerCanvas = [&](SceneProject* layerScene, Entity layerCamera, float& layerWidth, float& layerHeight) {
        layerWidth = (float)canvasWidth;
        layerHeight = (float)canvasHeight;
        CameraComponent* camera = layerScene->scene->findComponent<CameraComponent>(layerCamera);
        if (layerScene->sceneType == SceneType::SCENE_UI && layerScene->scene->getCanvasScale().scaled &&
            camera && camera->type == CameraType::CAMERA_UI) {
            const Vector2 ui = layerScene->scene->getSystem<UISystem>()->getCanvasSize();
            layerWidth = ui.x;
            layerHeight = ui.y;
        }
    };

    std::vector<Engine::ViewLayer> layers;
    float mainCanvasWidth = 0.0f, mainCanvasHeight = 0.0f;
    layerCanvas(&sceneProject, cameraEntity, mainCanvasWidth, mainCanvasHeight);
    layers.push_back({sceneProject.scene, cameraEntity, viewRect, false, true, false, mainCanvasWidth, mainCanvasHeight});
    layers.back().atmosphereQuality = gameWindow->getSettings().atmosphereQuality;

    for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
        SceneProject* childScene = project.getScene(childSceneRef.id);
        if (!childSceneRef.startActive || !childScene || !childScene->scene) continue;
        const Entity childCamera = project.getGameCamera(childSceneRef.id);
        if (childCamera == NULL_ENTITY) continue;

        // an inline child was updated and drawn this frame as a layer of the Scene view
        if (!childScene->expandedInline) {
            childScene->scene->getSystem<MeshSystem>()->update(0);
            childScene->scene->getSystem<UISystem>()->update(0);
            childScene->scene->getSystem<UIDocumentSystem>()->update(0);
            childScene->scene->getSystem<RenderSystem>()->update(0);
        }
        float childCanvasWidth = 0.0f, childCanvasHeight = 0.0f;
        layerCanvas(childScene, childCamera, childCanvasWidth, childCanvasHeight);
        layers.push_back({childScene->scene, childCamera, viewRect, false, false, false, childCanvasWidth, childCanvasHeight});
    }

    Engine::drawView(GAME_VIEW_ID, target, layers);

    gameWindow->markEditPassRendered(sceneProject.id);
    renderedSceneThisFrame = true;
}

void editor::App::renderGameViewPass(SceneProject& sceneProject) {
    const int width = gameWindow->getWidth();
    const int height = gameWindow->getHeight();
    const Entity cameraEntity = project.getGameCamera(sceneProject.id);
    if (width <= 0 || height <= 0 || cameraEntity == NULL_ENTITY) {
        return;
    }

    SceneRender* sceneRender = sceneProject.sceneRender;

    // Same layers as the game gets at start: the child scenes that start active,
    // each through its own game camera instead of its editor camera
    std::vector<Scene*> childLayers;
    std::vector<std::pair<Scene*, Entity>> childCameras;
    for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
        SceneProject* childScene = project.getScene(childSceneRef.id);
        if (!childSceneRef.startActive || !childScene || !childScene->scene) continue;
        const Entity childCamera = project.getGameCamera(childSceneRef.id);
        if (childCamera == NULL_ENTITY) continue;

        if (childScene->sceneRender) {
            childScene->sceneRender->hideAllGizmos();
        }
        childCameras.push_back({childScene->scene, childScene->scene->getCamera()});
        childScene->scene->setCamera(childCamera);
        childLayers.push_back(childScene->scene);
        // its own view draws its editor camera again
        childScene->needUpdateRender = true;
    }
    sceneRender->setChildSceneLayers(childLayers);

    sceneRender->beginGameViewPass(cameraEntity, gameWindow->getFramebuffer());
    // The next editor draw has to put its framebuffer, camera and layers back
    lastActivatedScene = NULL_PROJECT_SCENE;

    // The editor draws at native scale; the game view frames the canvas like play does
    const int editCanvasWidth = Engine::getPreferredCanvasWidth();
    const int editCanvasHeight = Engine::getPreferredCanvasHeight();
    const Scaling editScaling = Engine::getScalingMode();
    Engine::setCanvasSize(project.getCanvasWidth(), project.getCanvasHeight());
    Engine::setScalingMode(project.getScalingMode());

    Platform::setSizes(width, height);
    Engine::systemViewChanged();

    for (Scene* childScene : childLayers) {
        childScene->getSystem<MeshSystem>()->update(0);
        childScene->getSystem<UISystem>()->update(0);
        childScene->getSystem<UIDocumentSystem>()->update(0);
        childScene->getSystem<RenderSystem>()->update(0);
    }
    sceneRender->updateRenderSystem();
    // the Game view's atmosphere: the scene's fog and clouds whatever the Scene view's toggles
    // show, at its Preview Atmosphere Quality
    RenderSystem* renderSystem = sceneProject.scene->getSystem<RenderSystem>().get();
    renderSystem->setDisableFog(false);
    renderSystem->setDisableClouds(false);
    renderSystem->setPreviewAtmosphereQuality(gameWindow->getSettings().atmosphereQuality);

    Engine::systemDraw();
    renderSystem->keepAtmosphereInfo(GAME_VIEW_ID);

    Engine::setCanvasSize(editCanvasWidth, editCanvasHeight);
    Engine::setScalingMode(editScaling);

    sceneRender->endGameViewPass();
    for (const auto& [childScene, editorCamera] : childCameras) {
        childScene->setCamera(editorCamera);
    }
    gameWindow->markEditPassRendered(sceneProject.id);
    renderedSceneThisFrame = true;
}

void editor::App::tickBenchmark() {
    if (benchmarkExit)
        return;

    SceneProject* selected = project.getScene(benchmarkSceneId);
    if (!selected || !selected->scene || !selected->sceneRender) {
        if (benchmarkWaitStart.time_since_epoch().count() == 0)
            benchmarkWaitStart = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - benchmarkWaitStart > std::chrono::seconds(90)) {
            failBenchmark("timed out waiting for the scene to load");
        }
        return;
    }

    if (benchmarkPlay) {
        // Play first, then measure the running scene (scripts, animation, physics) once it plays
        if (!benchmarkPlayStarted) {
            project.start(benchmarkSceneId);
            benchmarkPlayStarted = true;
            benchmarkWaitStart = std::chrono::steady_clock::now();
            return;
        }
        if (selected->playState != ScenePlayState::PLAYING) {
            if (selected->playState == ScenePlayState::STOPPED || selected->playState == ScenePlayState::CANCELLING) {
                failBenchmark("the scene did not start playing (see the log)");
            } else if (std::chrono::steady_clock::now() - benchmarkWaitStart > std::chrono::seconds(90)) {
                failBenchmark("timed out waiting for the scene to start playing");
            }
            return;
        }
        // the resource wait below counts from the start of play, not from the start request
        if (!benchmarkCameraReady)
            benchmarkWaitStart = {};
    }

    if (!benchmarkCameraReady) {
        if (benchmarkPlay) {
            // the playing scene is the Game view's frame, through the scene's camera; in front,
            // it hides the Scene view (the same dock), which would draw a second view of it
            gameWindow->requestFocus();
            benchmarkCameraReady = true;
        } else {
            // the scene tab decides the selected scene, so bring it to front like Play does
            if (sceneWindow)
                sceneWindow->requestPlayFocus(benchmarkSceneId);
            Entity camera = selected->scene->getCamera();
            if (camera == NULL_ENTITY)
                camera = selected->mainCamera;
            if (selected->sceneRender->setPreviewCamera(camera))
                benchmarkCameraReady = true;
        }
        if (benchmarkMeshLod >= 0)
            selected->scene->setMeshLodEnabled(benchmarkMeshLod != 0);
        if (benchmarkGpuInstancing >= 0)
            Engine::setGpuInstancingEnabled(benchmarkGpuInstancing != 0);
        if (benchmarkDepthPrepass >= 0)
            selected->scene->setDepthPrepassEnabled(benchmarkDepthPrepass != 0);
        if (benchmarkLitQuality >= 0)
            selected->scene->setLitQuality(benchmarkLitQuality ? LitQuality::HIGH : LitQuality::STANDARD);
        selected->displaySettings.showGrid3D = false;
        selected->displaySettings.showOrigin = false;
        selected->displaySettings.hideSelectionOutline = true;
        selected->sceneRender->hideAllGizmos();
    }

    auto meshSystem = selected->scene->getSystem<MeshSystem>();
    auto renderSystem = selected->scene->getSystem<RenderSystem>();
    auto uiSystem = selected->scene->getSystem<UISystem>();
    const bool loading =
        Engine::getQueuedResourceCount() > 0 ||
        (meshSystem && (meshSystem->hasPendingAsyncModelLoads() || meshSystem->hasPendingFoliageUpdates())) ||
        (renderSystem && (!renderSystem->isAllLoaded() || renderSystem->hasPendingMeshLods())) ||
        (uiSystem && uiSystem->hasPendingImages());

    if (benchmarkPhase == 0) {
        if (benchmarkWaitStart.time_since_epoch().count() == 0)
            benchmarkWaitStart = std::chrono::steady_clock::now();
        if (loading) {
            if (std::chrono::steady_clock::now() - benchmarkWaitStart > std::chrono::seconds(90)) {
                std::cerr << "benchmark: timed out waiting for resources; measuring anyway\n";
                benchmarkPhase = 1;
                benchmarkPhaseFrames = 0;
            }
            return;
        }
        if (!renderedSceneThisFrame || (!benchmarkPlay && sceneWindow->getWidth(selected->id) <= 0))
            return;
        benchmarkPhase = 1;
        benchmarkPhaseFrames = 0;
        return;
    }

    if (!renderedSceneThisFrame)
        return;

    if (benchmarkPhase == 1) {
        // GPU timing runs from the warmup on, so its first (partial) frames are not measured
        if (benchmarkPhaseFrames == 0 && benchmarkGpuPasses)
            SystemRender::acquireGpuTiming();
        if (++benchmarkPhaseFrames >= benchmarkWarmupFrames) {
            benchmarkPhase = 2;
            benchmarkPhaseFrames = 0;
            benchmarkFps.clear();
            benchmarkDraws.clear();
            benchmarkRenderStats.clear();
            benchmarkRenderCpuMs.clear();
            benchmarkInstances.clear();
            benchmarkTris.clear();
            benchmarkUiTotals.clear();
            benchmarkCpuMs.clear();
            benchmarkGpuFrameMs.clear();
            benchmarkGpuPassList.clear();
            // the frames drawn from the next one on are the measured ones
            benchmarkFirstFrame = FrameTimes::currentFrame() + 1;
            // --profile: the measured frames record (the profiler's overhead, tools/perf/profiler_overhead.py)
            if (benchmarkProfile)
                profiler::setEnabled(true);
            benchmarkGpuCollected = benchmarkFirstFrame - 1;
            if (!benchmarkCapturePath.empty()) {
                if (!benchmarkGpuPasses)
                    SystemRender::acquireGpuTiming();
                profiler::setGpuSemantics(SystemRender::getGpuTimingState().semantics);
                benchmarkCaptureId = profiler::startCapture((uint32_t)std::min(benchmarkMeasureFrames, 600));
            }
            benchmarkMeasureStart = std::chrono::steady_clock::now();
        }
        return;
    }

    if (benchmarkPhase == 2) {
        const Engine::FrameStats& stats = Engine::getFrameStats();
        benchmarkFps.push_back(Engine::getFramerate());
        benchmarkDraws.push_back(stats.drawCalls);
        benchmarkInstances.push_back(stats.instances);
        benchmarkTris.push_back(stats.triangles);
        benchmarkRenderStats.push_back(SystemRender::getRenderStats(SystemRender::FRAME_STATS_FRAME));
        benchmarkRenderCpuMs.push_back(Engine::getFrameDrawCpuTime() * 1000.0);
        if (benchmarkUiStats) {
            // the scene's documents (screen and texture-mode contexts), this frame
            std::shared_ptr<UIDocumentSystem> documents = selected->scene->getSystem<UIDocumentSystem>();
            const HtmlUIRenderStats ui = documents ? documents->getRenderStats() : HtmlUIRenderStats();
            size_t field = 0;
            ui.forEachField([this, &field](const char* name, uint32_t value) {
                if (field == benchmarkUiTotals.size())
                    benchmarkUiTotals.emplace_back(name, 0.0);
                benchmarkUiTotals[field++].second += (double)value;
            });
        }
        collectBenchmarkGpuFrame();
        if (static_cast<int>(benchmarkFps.size()) >= benchmarkMeasureFrames) {
            benchmarkLastFrame = FrameTimes::currentFrame();
            benchmarkPhase = 3;
            benchmarkDrainFrames = 0;
        }
        return;
    }

    if (benchmarkPhase == 3) {
        // the GPU times of the last measured frames, and the capture, arrive a few frames late
        collectBenchmarkGpuFrame();
        profiler::CaptureInfo capture;
        const bool captureDone = !benchmarkCaptureId || !profiler::captureInfo(benchmarkCaptureId, capture) || capture.complete;
        const bool gpuDone = !benchmarkGpuPasses || benchmarkGpuCollected >= benchmarkLastFrame;
        if ((captureDone && gpuDone) || ++benchmarkDrainFrames > 60)
            finishBenchmark();
    }
}

// The newest frame whose GPU times all arrived, once, while it is a measured one: each pass
// label's time (its passes summed) and count, and the frame's GPU time
void editor::App::collectBenchmarkGpuFrame() {
    if (!benchmarkGpuPasses)
        return;
    const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, true);
    if (list.frame <= benchmarkGpuCollected || list.frame < benchmarkFirstFrame || list.gpuPending)
        return;
    if (benchmarkLastFrame && list.frame > benchmarkLastFrame)
        return;
    bool timed = false;
    for (const PassRecord& pass : list.passes)
        timed = timed || pass.gpuValid;
    if (!timed)
        return;
    benchmarkGpuCollected = list.frame;
    if (list.gpuFrameValid)
        benchmarkGpuFrameMs.push_back(list.gpuFrameMs);
    const size_t sampleIndex = benchmarkGpuFrameMs.size() - (list.gpuFrameValid ? 1 : 0);
    for (BenchmarkPass& pass : benchmarkGpuPassList) {
        pass.frameMs.resize(sampleIndex + 1, 0.0);
        pass.counts.resize(sampleIndex + 1, 0.0);
    }
    for (const PassRecord& pass : list.passes) {
        if (!pass.gpuValid)
            continue;
        auto it = std::find_if(benchmarkGpuPassList.begin(), benchmarkGpuPassList.end(),
                               [&](const BenchmarkPass& entry) { return entry.label == pass.label; });
        if (it == benchmarkGpuPassList.end()) {
            BenchmarkPass entry;
            entry.label = pass.label;
            entry.frameMs.assign(sampleIndex + 1, 0.0);
            entry.counts.assign(sampleIndex + 1, 0.0);
            benchmarkGpuPassList.push_back(entry);
            it = benchmarkGpuPassList.end() - 1;
        }
        it->frameMs[sampleIndex] += pass.gpuMs;
        it->counts[sampleIndex] += 1.0;
    }
}

void editor::App::finishBenchmark() {
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - benchmarkMeasureStart).count();
    const size_t n = benchmarkFps.size();
    double avgFps = 0.0;
    double avgDraws = 0.0;
    double avgInstances = 0.0;
    double avgTris = 0.0;
    for (size_t i = 0; i < n; i++) {
        avgFps += benchmarkFps[i];
        avgDraws += benchmarkDraws[i];
        avgInstances += benchmarkInstances[i];
        avgTris += static_cast<double>(benchmarkTris[i]);
    }
    if (n > 0) {
        avgFps /= static_cast<double>(n);
        avgDraws /= static_cast<double>(n);
        avgInstances /= static_cast<double>(n);
        avgTris /= static_cast<double>(n);
    }

    float minFps = 0.0f;
    float maxFps = 0.0f;
    if (!benchmarkFps.empty()) {
        minFps = *std::min_element(benchmarkFps.begin(), benchmarkFps.end());
        maxFps = *std::max_element(benchmarkFps.begin(), benchmarkFps.end());
    }

    const double wallFps = (elapsed > 0.0 && n > 0) ? static_cast<double>(n) / elapsed : 0.0;

    // the measured frames' CPU time (FrameTimes: the editor loop's work, presenting excluded)
    benchmarkCpuMs.clear();
    for (const FrameTimeSample& sample : FrameTimes::snapshot()) {
        if (sample.frame >= benchmarkFirstFrame && (benchmarkLastFrame == 0 || sample.frame <= benchmarkLastFrame))
            benchmarkCpuMs.push_back(sample.cpuMs);
    }
    auto stat = [](std::vector<double> values, double p) {
        if (values.empty())
            return 0.0;
        std::sort(values.begin(), values.end());
        const double position = p * static_cast<double>(values.size() - 1);
        const size_t low = static_cast<size_t>(position);
        const size_t high = std::min(low + 1, values.size() - 1);
        return values[low] + (values[high] - values[low]) * (position - static_cast<double>(low));
    };
    auto mean = [](const std::vector<double>& values) {
        double sum = 0.0;
        for (double value : values)
            sum += value;
        return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
    };

    SceneProject* selected = project.getScene(benchmarkSceneId);
    // the view measured: the Scene view's, or the Game view's while playing
    const int width = benchmarkPlay ? gameWindow->getRenderWidth() : (selected && sceneWindow ? sceneWindow->getWidth(selected->id) : 0);
    const int height = benchmarkPlay ? gameWindow->getRenderHeight() : (selected && sceneWindow ? sceneWindow->getHeight(selected->id) : 0);

    std::ostringstream json;
    json << std::fixed;
    json.precision(2);
    json << "{\n"
         << "  \"name\": \"" << jsonEscape(benchmarkName.empty() ? (selected ? selected->name : "scene") : benchmarkName) << "\",\n"
         << "  \"scene\": \"" << jsonEscape(selected ? selected->name : benchmarkScene) << "\",\n"
         << "  \"meshLod\": " << ((selected && selected->scene && selected->scene->isMeshLodEnabled()) ? "true" : "false") << ",\n"
         << "  \"depthPrepass\": " << ((selected && selected->scene && selected->scene->isDepthPrepassEnabled()) ? "true" : "false") << ",\n"
         << "  \"play\": " << (benchmarkPlay ? "true" : "false") << ",\n"
         << "  \"viewport\": [" << width << ", " << height << "],\n"
         << "  \"frames\": " << n << ",\n"
         << "  \"warmupFrames\": " << benchmarkWarmupFrames << ",\n"
         << "  \"elapsedSeconds\": " << elapsed << ",\n"
         << "  \"wallFps\": " << wallFps << ",\n"
         << "  \"avgFps\": " << avgFps << ",\n"
         << "  \"minFps\": " << minFps << ",\n"
         << "  \"maxFps\": " << maxFps << ",\n"
         << "  \"avgMs\": " << (wallFps > 0.0 ? 1000.0 / wallFps : 0.0) << ",\n"
         << "  \"avgDrawCalls\": " << avgDraws << ",\n"
         << "  \"avgInstances\": " << avgInstances << ",\n"
         << "  \"avgTriangles\": " << avgTris << ",\n"
         << "  \"profiling\": " << (benchmarkProfile ? "true" : "false") << ",\n";
    json.precision(3);
    json << "  \"cpuMs\": {\"median\": " << stat(benchmarkCpuMs, 0.5) << ", \"p95\": " << stat(benchmarkCpuMs, 0.95)
         << ", \"mean\": " << mean(benchmarkCpuMs) << "}";
    // the scenes' draw and the commit of each measured frame (the render submission, S55 R9)
    json << ",\n  \"renderCpuMs\": {\"median\": " << stat(benchmarkRenderCpuMs, 0.5) << ", \"p95\": " << stat(benchmarkRenderCpuMs, 0.95)
         << ", \"mean\": " << mean(benchmarkRenderCpuMs) << "}";
    {
        // the game frame's render counters averaged over the measured frames (S55 R7; with --play
        // the frame the Game view shows): deterministic for a still scene
        const struct { const char* name; double (*value)(const RenderStats&); } counters[] = {
            {"mainDrawCalls", [](const RenderStats& r) { return (double)r.mainDraws; }},
            {"shadowDrawCalls", [](const RenderStats& r) { return (double)r.shadowDraws; }},
            {"depthDrawCalls", [](const RenderStats& r) { return (double)r.depthDraws; }},
            {"otherDrawCalls", [](const RenderStats& r) { return (double)r.otherDraws; }},
            {"mainTriangles", [](const RenderStats& r) { return (double)r.mainTriangles; }},
            {"shadowTriangles", [](const RenderStats& r) { return (double)r.shadowTriangles; }},
            {"instancedDraws", [](const RenderStats& r) { return (double)r.instancedDraws; }},
            {"batchedInstances", [](const RenderStats& r) { return (double)r.batchedInstances; }},
            {"visible", [](const RenderStats& r) { return (double)r.visible; }},
            {"culled", [](const RenderStats& r) { return (double)r.culled; }},
            {"uniformBytes", [](const RenderStats& r) { return (double)r.uniformBytes; }},
            {"uniformBytesKept", [](const RenderStats& r) { return (double)r.uniformBytesKept; }},
            {"bufferUploadBytes", [](const RenderStats& r) { return (double)r.bufferUploadBytes; }},
            {"pipelinesApplied", [](const RenderStats& r) { return (double)r.pipelinesApplied; }},
            {"bindingsApplied", [](const RenderStats& r) { return (double)r.bindingsApplied; }},
            {"aliveBuffers", [](const RenderStats& r) { return (double)r.aliveBuffers; }},
            {"aliveImages", [](const RenderStats& r) { return (double)r.aliveImages; }},
            {"alivePipelines", [](const RenderStats& r) { return (double)r.alivePipelines; }},
        };
        json << ",\n  \"render\": {";
        bool first = true;
        for (const auto& counter : counters) {
            double sum = 0.0;
            for (const RenderStats& frameStats : benchmarkRenderStats)
                sum += counter.value(frameStats);
            json << (first ? "" : ", ") << "\"" << counter.name << "\": "
                 << (benchmarkRenderStats.empty() ? 0.0 : sum / static_cast<double>(benchmarkRenderStats.size()));
            first = false;
        }
        json << "}";
        // GPU Instancing in the last measured frame: the draws that could batch, and why one did
        // not join the batch before it (S55 R7)
        const DrawBatchStats batch = SystemRender::getDrawBatchStats();
        json << ",\n  \"drawBatch\": {\"offered\": " << batch.offeredDraws << ", \"instancedDraws\": " << batch.instancedDraws
             << ", \"batchedInstances\": " << batch.batchedInstances << ", \"splitPipeline\": " << batch.splitPipeline
             << ", \"splitElements\": " << batch.splitElements << ", \"splitBindings\": " << batch.splitBindings
             << ", \"splitUniforms\": " << batch.splitUniforms << "}";
    }
    if (benchmarkUiStats) {
        // HtmlUIRenderStats averaged over the measured frames
        json << ",\n  \"htmlUi\": {";
        for (size_t i = 0; i < benchmarkUiTotals.size(); i++) {
            json << (i ? ", " : "") << "\"" << benchmarkUiTotals[i].first << "\": "
                 << (n > 0 ? benchmarkUiTotals[i].second / static_cast<double>(n) : 0.0);
        }
        json << "}";
    }
    if (benchmarkGpuPasses) {
        const GpuTimingState gpu = SystemRender::getGpuTimingState();
        json << ",\n  \"gpu\": {\n"
             << "    \"available\": " << (gpu.available ? "true" : "false") << ",\n"
             << "    \"reason\": \"" << jsonEscape(gpu.reason) << "\",\n"
             << "    \"backend\": \"" << jsonEscape(SystemRender::getBackendName()) << "\",\n"
             << "    \"device\": \"" << jsonEscape(SystemRender::getDeviceName()) << "\",\n"
             << "    \"semantics\": \"" << jsonEscape(gpu.semantics) << "\",\n"
             << "    \"frameIsSum\": " << (gpu.frameIsSum ? "true" : "false") << ",\n"
             << "    \"frames\": " << benchmarkGpuFrameMs.size() << ",\n"
             << "    \"frameMs\": {\"median\": " << stat(benchmarkGpuFrameMs, 0.5) << ", \"p95\": " << stat(benchmarkGpuFrameMs, 0.95)
             << ", \"mean\": " << mean(benchmarkGpuFrameMs) << "},\n"
             << "    \"passes\": [";
        for (size_t i = 0; i < benchmarkGpuPassList.size(); i++) {
            const BenchmarkPass& pass = benchmarkGpuPassList[i];
            json << (i ? ",\n" : "\n") << "      {\"label\": \"" << jsonEscape(pass.label) << "\", \"perFrame\": " << stat(pass.counts, 0.5)
                 << ", \"medianMs\": " << stat(pass.frameMs, 0.5) << ", \"p95Ms\": " << stat(pass.frameMs, 0.95)
                 << ", \"meanMs\": " << mean(pass.frameMs) << "}";
        }
        json << (benchmarkGpuPassList.empty() ? "]\n" : "\n    ]\n") << "  }";
        SystemRender::releaseGpuTiming();
    }
    if (benchmarkCaptureId) {
        std::string error;
        if (profiler::saveCapture(benchmarkCaptureId, benchmarkCapturePath.string(), &error)) {
            json << ",\n  \"profileCapture\": \"" << jsonEscape(benchmarkCapturePath.string()) << "\"";
            std::cerr << "Profiler capture written: " << benchmarkCapturePath.string() << "\n";
        } else {
            failBenchmark("the profiler capture was not written: " + error);
        }
        if (!benchmarkGpuPasses)
            SystemRender::releaseGpuTiming();
    }
    json << "\n}\n";

    const std::string text = json.str();
    std::cout << text;
    std::cout.flush();
    if (!benchmarkOut.empty()) {
        std::ofstream file(benchmarkOut);
        if (file)
            file << text;
        else
            failBenchmark("failed to write " + benchmarkOut.string());
    }

    benchmarkEnabled = false;
    benchmarkExit = true;
}

void editor::App::enqueueMainThreadTask(std::function<void()> task) {
    if (!task) return;
    scheduler().post(std::move(task));
    // Wake an idle backend loop so cross-thread work runs promptly.
    std::lock_guard<std::mutex> lock(wakeMutex);
    if (wakeCallback) wakeCallback();
}

bool editor::App::hasPendingMainThreadTasks() {
    return scheduler().hasPosted();
}

double editor::App::idleWaitSeconds(double maxSeconds) const {
    const double next = scheduler().nextDueRealTime();
    double wait = next < 0.0 ? maxSeconds : std::min(maxSeconds, next);
    // a hidden game's next update (runHiddenGameFrame); a paused or held one keeps the idle sleep
    if (isHiddenGameRunning())
        wait = std::min(wait, hiddenGameClock.secondsUntilDue());
    return wait;
}

// Counts as activity, so the loop also keeps drawing for the idle delay past the
// last request: enough for ImGui to auto-resize a window whose content just grew.
void editor::App::recordFrameTime(double ms) {
    if (frameTimes.size() < 100000) frameTimes.push_back(static_cast<float>(ms));
}

nlohmann::json editor::App::takeFrameStats() {
    std::vector<float> times;
    times.swap(frameTimes);
    if (times.empty()) return {{"median", 0}, {"p99", 0}, {"max", 0}, {"frames", 0}};
    std::sort(times.begin(), times.end());
    auto round1 = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    const size_t p99 = std::min(times.size() - 1, static_cast<size_t>(std::ceil(times.size() * 0.99)) - 1);
    return {{"median", round1(times[times.size() / 2])}, {"p99", round1(times[p99])}, {"max", round1(times.back())}, {"frames", times.size()}};
}

void editor::App::requestRedraw() {
    redrawRequested = true;
}

bool editor::App::consumeRedrawRequest() {
    const bool requested = redrawRequested;
    redrawRequested = false;
    return requested;
}

void editor::App::requestFrame() {
    frameRequested = true;
}

void editor::App::requestContinuousFrames(double seconds) {
    const auto until = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    if (until > continuousFramesUntil)
        continuousFramesUntil = until;
}

bool editor::App::wantsContinuousFrames() const {
    if (profiler::activeCapture() != 0)
        return true;
    // every editor frame is one game frame of a recording (S27 R27)
    if (recorder::RecorderSession::get().active())
        return true;
    return std::chrono::steady_clock::now() < continuousFramesUntil;
}

bool editor::App::consumeFrameRequest() {
    const bool requested = frameRequested;
    frameRequested = false;
    return requested;
}

void editor::App::setWakeCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(wakeMutex);
    wakeCallback = std::move(cb);
}

void editor::App::shutdownBackgroundWork() {
    // clip previews restored while the scenes still exist (nothing of them runs at static exit)
    if (animationClipAppHooks().projectClosed) {
        animationClipAppHooks().projectClosed();
    }
    // teardown is watched on its own window (the job system is not read from here on)
    EditorDiagnostics::beginShutdown();
    // the window layout as it is, for the next start
    layouts.saveWorkingLayoutNow();
    // the Project Hub's picture of the project (S27 R21)
    writeHubThumbnail();
    // a clean quit: this session's crash backups and the project lock go
    project.endProjectSession();
    loadingPump = nullptr;
    pendingProjectChange = nullptr;
    // Before glfwTerminate/SDL_Quit: pending MCP calls fail, the HTTP listener closes.
    mcp::McpServer::get().stop();
    if (!editor::texsynth::SynthesisRun::cancelAll()) {
        Out::warning("A texture synthesis did not stop in time; it is left to finish without effect.");
    }
    HotReloadService::get().stopWatching();
    if (spriteNormalsWatcher) spriteNormalsWatcher->shutdown(1.0);
    // Shift-click generations and Regenerates: cancelled, waited for a moment
    if (spriteNormalsWindow) spriteNormalsWindow->shutdownJobs(2.0);
    if (!editor::spritenormals::SpriteNormalsRun::cancelAll()) {
        Out::warning("A sprite normal map generation did not stop in time; it is left to finish without effect.");
    }
    htmlui::HtmlUiAutoImport::get().shutdown();
    setWakeCallback(nullptr);
    PoseRender::setFrameRequester(nullptr);
    ParticleEffectPreview::setFrameRequester(nullptr);
    procmat::ProceduralRun::setFrameRequester(nullptr);
}

void editor::App::engineViewDestroyed(){
    imageViewerWindow->closeAll();
    // no thumbnail step renders off the main thread (and commits to the render queue) from
    // here on, nor touches the scenes the view's destruction removes
    projectWindow->stopThumbnailWork();
    // the offscreen pose renderers' private scenes are not the Engine's: they free their GPU
    // objects here, while the view still exists (not on Engine::onViewDestroyed, which a play
    // session's stop fires too)
    PoseRender::releaseGpu();
    VatPreviewView::releaseGpu();
    ParticleEffectPreview::releaseGpu();
    procmat::ProceduralRun::releaseGpu();
    // Closing during project loading can leave uploads committed before any scene frame
    // consumes them. Drain that batch before shutdown waits to flush the queue.
    SystemRender::executeQueue();
    Engine::systemViewDestroyed();
}

void editor::App::engineShutdown(){
    ::jasperengine::htmlui::EffectRegistry::get().setProjectEffectListener(nullptr);
    Engine::systemShutdown();
    // a clean exit: the watchdog stops and the session marker goes
    EditorDiagnostics::end();
}

void editor::App::addNewSceneToDock(uint32_t sceneId){
    if (isInitialized && !projectLoading){
        const std::string windowName = "###Scene" + std::to_string(sceneId);
        const SceneProject* sceneProject = project.getScene(sceneId);
        const bool isUnsavedScene = sceneProject && sceneProject->filepath.empty();

        dockTabWindow(windowName, isUnsavedScene);
    }
}

void editor::App::sceneDeleting(uint32_t sceneId) {
    if (physicsPlacementWindow) {
        physicsPlacementWindow->sceneDeleting(sceneId);
    }
}

void editor::App::clearSceneWindowState(uint32_t sceneId) {
    if (sceneWindow) {
        sceneWindow->clearSceneState(sceneId);
    }

    if (inspectorWindow) {
        inspectorWindow->clearSceneState(sceneId);
    }

    if (animationWindow) {
        animationWindow->clearSceneState(sceneId);
    }
    if (animationClipAppHooks().sceneClosed) {
        animationClipAppHooks().sceneClosed(sceneId);
    }

    if (poseAnimationWindow) {
        poseAnimationWindow->clearSceneState(sceneId);
    }

    if (animatorWindow) {
        animatorWindow->clearSceneState(sceneId);
    }

    if (particleEffectWindow) {
        particleEffectWindow->clearSceneState(sceneId);
    }

    if (avatarWindow) {
        avatarWindow->clearSceneState(sceneId);
    }

    if (lastActivatedScene == sceneId) {
        resetLastActivatedScene();
    }
}

void editor::App::prepareForProjectSwitch() {
    inspectorWindow->resetMaterialAsset();
    // the Project Hub's picture of the project closing (S27 R21)
    writeHubThumbnail();
    // the project's working layout, before another project's replaces it (S25 R19)
    layouts.saveWorkingLayoutNow();
    // the watcher stops before the project goes (S05 D2)
    HotReloadService::get().stopWatching();
    // the posed bones go back while their scenes still exist
    poseAnimationWindow->prepareForProjectSwitch();
    // the Animator window's preview goes back, its controller document is dropped
    animatorWindow->prepareForProjectSwitch();
    avatarWindow->stopPreview();
    avatarWindow->projectClosed();
    // and the clip previews (documents and the shared clip controller are released)
    if (animationClipAppHooks().projectClosed) {
        animationClipAppHooks().projectClosed();
    }
    imageViewerWindow->closeAll();
    // a synthesis started in this project is cancelled and never written into the next one
    textureSynthesisWindow->resetForProjectSwitch();
    audioMixerWindow->resetForProjectSwitch();
    particleEffectWindow->resetForProjectSwitch();
    soundBrowserWindow->resetForProjectSwitch();
    GraphEditorWindow::prepareAllForProjectSwitch();
    audioCapturesWindow->resetForProjectSwitch();
    // a live preview or a Shift-click generation is cancelled, its sources and results forgotten
    spriteNormalsWindow->resetForProjectSwitch();
    spriteNormalsWatcher->reset();
    editor::spritenormals::SpriteNormalsRun::cancelRunning();
    // a running bake or import is cancelled, its source and result forgotten
    vertexAnimationBakerWindow->resetForProjectSwitch();
    editor::texsynth::projectSwitching();
    // physics gestures are cancelled, the sessions and the decomposition cache dropped
    physicsPlacementWindow->resetForProjectSwitch();
    projectWindow->cancelThumbnailWork();
    // Thumbnail cancellation prevents new preview loads from being queued;
    // now quiesce any remaining scene/model jobs before pool cleanup.
    MeshSystem::cancelAllAsyncModelLoads();
    // Project deletes these scenes and framebuffers before another scene frame runs.
    Engine::removeAllScenes();
    Engine::setFramebuffer(nullptr);
    codeEditor->closeAll();
    consoleWindow->clear();
    sceneWindow->resetProjectState();
    gameWindow->resetProjectState();
    // the next project's Terrain settings are read before the window writes any back
    terrainEditWindow->resetProjectState();
    inputActionsWindow->reset();
    // the next project's .particles files are other files under the same paths
    particles::ParticleEffectLibrary::clear();
    ParticleMigrationReport::clear();
    dockspaceNeedsRebuild = true;
}

std::string editor::App::backgroundWorkConflict(const std::string& action) const {
    mcp::McpServer& server = mcp::McpServer::get();
    if (!server.isStarted()) return {};
    // A human action: the export window's own export is not a conflict for itself
    const mcp::ToolActivity activity = action == "play" ? mcp::ToolActivity::PlayStart : mcp::ToolActivity::Build;
    return server.busyRefusal(activity, nullptr, false);
}

void editor::App::requestScenePlayFocus(uint32_t sceneId) {
    if (!isMainThread()) {
        enqueueMainThreadTask([this, sceneId]() {
            requestScenePlayFocus(sceneId);
        });
        return;
    }

    // Unity brings the Game view forward when play starts; a closed one opens
    if (gameWindow && project.getPlaySessionSceneId() == sceneId) {
        project.setSelectedSceneId(sceneId);
        gameWindow->requestFocus();
        return;
    }

    if (sceneWindow) {
        sceneWindow->requestPlayFocus(sceneId);
    }
}

void editor::App::addNewCodeWindowToDock(fs::path path, bool force){
    if (isInitialized && !projectLoading){
        dockTabWindow("###" + path.string(), force);
    }
}

void editor::App::addImageViewerWindowToDock(fs::path path, bool force){
    if (isInitialized && !projectLoading){
        dockTabWindow(ImageViewerWindow::getWindowId(path), force);
    }
}

void editor::App::handleExternalDrop(const std::vector<std::string>& paths) {
    isDroppedExternalPaths = true;
    droppedExternalPaths = paths;
}

void editor::App::handleExternalDragEnter() {
    projectWindow->handleExternalDragEnter();
}

void editor::App::handleExternalDragLeave() {
    projectWindow->handleExternalDragLeave();
}

void editor::App::resetLastActivatedScene(){
    lastActivatedScene = NULL_PROJECT_SCENE;
    lastActivatedGame = false;
    pendingResizeScene = NULL_PROJECT_SCENE;
}

bool editor::App::shouldSyncEngineApi() const {
    return true;
}

bool editor::App::recordsRecentProjects() const {
    const mcp::McpLaunchOptions& options = mcp::mcpLaunchOptions();
    return !options.stdio && !options.headless && !benchmarkEnabled;
}

void editor::App::updateResourcesPath(){
    std::filesystem::path currentProjectPath = project.getProjectPath().lexically_normal();
    bool projectPathChanged = lastResourcesProjectPath != currentProjectPath;

    if (isInitialized){
        if (projectPathChanged) {
            projectWindow->notifyProjectPathChange();
        } else {
            projectWindow->refreshCurrentDirectory();
        }
    }

    // Initial project loading happens before the Project window is initialized,
    // but prepareForProjectSwitch() still suspends its worker.
    projectWindow->resumeThumbnailWork();

    lastResourcesProjectPath = currentProjectPath;
    projectWindow->cleanupThumbnails();
}

void editor::App::updateWindowTitle(const std::string& projectName) {
    windowTitleProject = projectName;
    Backend::updateWindowTitle(projectName + windowTitleSuffix);
}

void editor::App::stopTransientPreviews() {
    // before a scene save or play: posed bones go back to their saved transforms
    if (poseAnimationWindow) {
        poseAnimationWindow->stopTransientPreviews();
    }
    // the Animator window's edit-mode preview (the bones go back)
    if (animatorWindow) {
        animatorWindow->stopPreview();
    }
    if (avatarWindow) {
        avatarWindow->stopTransientPreviews();
    }
    // and the Animation window's previewed targets to their snapshots (animation clips 9.R6):
    // the entity-sequence mode, then the clip mode
    if (animationWindow) {
        animationWindow->stopTransientPreviews();
    }
    if (animationClipAppHooks().stopTransientPreviews) {
        animationClipAppHooks().stopTransientPreviews();
    }
    if (terrainEditWindow) {
        terrainEditWindow->endStroke();
        terrainEditWindow->updateFoliagePreview();
    }
    if (inspectorWindow) {
        inspectorWindow->stopTransientPreviews();
    }
    // a scene is never saved or played with physics preview poses in it: a released gesture
    // settles and commits first, a held one is cancelled (and place_with_physics screenshots
    // put their poses back)
    if (physicsPlacementWindow) {
        physicsPlacementWindow->finaliseGestures();
    }
    mcp::stopPlacementPreviews(&project);
    mcp::stopAnimatorPreviews(&project);
}

bool editor::App::dispatchClipDrop(uint32_t sceneId, Entity target, const std::vector<std::string>& paths) {
    if (!animationClipAppHooks().dropClips) return false;
    return animationClipAppHooks().dropClips(&project, sceneId, target, paths);
}

void editor::App::flushSceneMaterialWrites(uint32_t sceneId) {
    if (inspectorWindow) {
        inspectorWindow->flushDirtyMaterialsForScene(sceneId);
    }
}

void editor::App::saveAllCodeEditors() {
    if (codeEditor) {
        codeEditor->saveAll();
    }
}

void editor::App::importUnityParticles() {
    const std::string file = FileDialogs::openFileDialog("", FILE_DIALOG_UNITY_PREFAB_SCENE);
    if (file.empty()) return;
    unity::ImportOptions options;
    options.file = fs::u8path(file);
    const unity::ImportResult result = unity::importParticlesIntoProject(&project, options);
    if (!result.ok) {
        Out::error("Import Unity Particle System: %s", result.error.c_str());
        registerAlert("Import Unity Particle System", result.error);
        return;
    }
    for (const std::string& prefab : result.prefabs) {
        Out::info("Imported Unity particles: %s", prefab.c_str());
    }
    // the report's notes, one line each
    for (const auto& system : result.report["systems"]) {
        for (const auto& note : system["notes"]) {
            Out::warning("%s: %s: %s", system["path"].get<std::string>().c_str(), note["code"].get<std::string>().c_str(),
                         note["message"].get<std::string>().c_str());
        }
    }
    for (const auto& warning : result.report["warnings"]) {
        Out::warning("Import Unity Particle System: %s", warning.get<std::string>().c_str());
    }
    if (!result.reportFile.empty()) {
        Out::info("Import report: %s", result.reportFile.c_str());
    }
}

void editor::App::registerAlert(std::string title, std::string message) {
    registerAlert(title, message, "");
}

void editor::App::registerAlert(std::string title, std::string message, std::string note) {
    alert.needShow = true;
    alertSerial++;
    alertPendingChoice.clear();
    alert.title = title;
    alert.message = message;
    alert.note = note;
    alert.type = AlertType::Info;
    alert.onYes = nullptr;
    alert.onNo = nullptr;
}

void editor::App::registerOutsideAssetsAlert(const std::string& path) {
    registerAlert(
        "File outside the assets directory",
        "Only files inside \"" + project.getAssetsDir().generic_string() + "\" can be used.\n"
        "Move or copy it there and drop it again.",
        project.normalizeToProjectRelative(path).generic_string());
}

void editor::App::registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo) {
    alert.needShow = true;
    alertSerial++;
    alertPendingChoice.clear();
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::Confirm;
    alert.onYes = onYes;
    alert.onNo = onNo;
}

void editor::App::registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel) {
    alert.needShow = true;
    alertSerial++;
    alertPendingChoice.clear();
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::ThreeButton;
    alert.yesLabel = "Yes";
    alert.noLabel = "No";
    alert.onYes = onYes;
    alert.onNo = onNo;
    alert.onCancel = onCancel;
}

void editor::App::registerSaveDiscardAlert(std::string title, std::string message, std::function<void()> onSave, std::function<void()> onDiscard, std::function<void()> onCancel) {
    registerThreeButtonAlert(std::move(title), std::move(message), std::move(onSave), std::move(onDiscard), std::move(onCancel));
    alert.yesLabel = "Save";
    alert.noLabel = "Discard Changes";
}

void editor::App::openInputActions(const std::filesystem::path& path) {
    inputActionsWindow->openFile(path);
    ImGui::SetWindowFocus(InputActionsWindow::WINDOW_NAME);
}

void editor::App::openPrefabAsset(const std::filesystem::path& path, uint32_t contextSceneId, uint32_t contextRoot, bool inContext) {
    // Next frame: the window asking is in the middle of drawing the scene it may replace
    enqueueMainThreadTask([this, path, contextSceneId, contextRoot, inContext]() {
        project.openPrefabStage(path, contextSceneId, contextRoot, inContext);
    });
}

// An editor serving an MCP client on stdio has nobody to answer a modal: it decides like a
// headless host (Refuse, Keep, no) and the agent passes allow_data_loss / recovery / --ignore-lock
editor::DegradedSavePolicy editor::App::confirmDegradedSave(const DegradedSaveRequest& request) {
    // the modal's Save Anyway runs the save again; this attempt writes nothing
    if (!mcp::mcpLaunchOptions().stdio) {
        dataSafetyDialogs.requestDegradedSave(request);
    }
    return DegradedSavePolicy::Refuse;
}

editor::RecoveryChoice editor::App::resolveSceneRecovery(const SceneBackupInfo& backup) {
    // the saved file opens now; the modal's Restore reloads the scene from the backup
    if (!mcp::mcpLaunchOptions().stdio) {
        dataSafetyDialogs.requestRecovery(backup);
    }
    return RecoveryChoice::Keep;
}

bool editor::App::confirmOpenLockedProject(const ProjectLockInfo& info) {
    if (!mcp::mcpLaunchOptions().stdio) {
        dataSafetyDialogs.requestLockedProject(info);
    }
    return false;
}

void editor::App::registerSaveSceneDialog(uint32_t sceneId, std::function<void(bool)> callback) {
    // Add scene to the save dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Scene, sceneId, callback};
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

void editor::App::registerProjectSaveDialog(std::function<void(bool)> callback) {
    // Add project save to the dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Project, 0, callback};  // sceneId is unused for Project saves
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

std::filesystem::path editor::App::getUserCacheBaseDir() {
    // Cache the result to avoid repeated syscalls/env lookups
    static std::filesystem::path cached = []() -> std::filesystem::path {
    #if defined(_WIN32)
        // Ensure COM is initialized for SHGetKnownFolderPath
        // Using COINIT_APARTMENTTHREADED is safe for most GUI apps
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // hrInit == S_OK means we initialized, RPC_E_CHANGED_MODE means already initialized differently (OK)
        // We don't call CoUninitialize here since we want COM available for the app lifetime

        PWSTR widePath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &widePath)) && widePath) {
            std::filesystem::path p(widePath);
            CoTaskMemFree(widePath);
            return p; // e.g. C:\Users\<you>\AppData\Local
        }
        // Fallback to environment variable
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
            return std::filesystem::path(localAppData);
        }
        return std::filesystem::temp_directory_path();

    #elif defined(__APPLE__)
        // Conventional macOS cache location: ~/Library/Caches
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / "Library" / "Caches";
        }
        return std::filesystem::temp_directory_path();

    #else
        // Linux / other Unix: XDG Base Dir spec
        // Prefer $XDG_CACHE_HOME, fallback to ~/.cache
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            return std::filesystem::path(xdg);
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / ".cache";
        }
        return std::filesystem::temp_directory_path();
    #endif
    }();

    return cached;
}

std::filesystem::path editor::App::getUserShaderCacheDir(){
    // Bump this version on any shader source change to invalidate stale cached
    // .sdat (the cache is keyed by shaderType+properties, not source content).
    // v4: mirror (USE_MIRROR) variants changed the u_fs_mirror block guard.
    // v5: SSAO (mesh.frag USE_SSAO + viewportInfo, new ssao/ssao_blur shaders).
    // v6: SSR (G-buffer geometry pass + ssr/ssr_blur/composite fullscreen shaders;
    //     energy-conserving SSR-over-IBL, glossy roughness blur, debug modes).
    // v7: second UV set (HAS_UV_SET2) — per-texture UV selection via u_fs_texCoordSets.
    // v8: directional/spot shadows share u_shadowAtlas (replaces u_shadowMap1..6),
    //     3x3 atlas, SSAO on terrain (USE_SSAO + depth HAS_TERRAIN).
    // v9: dual shadow atlases — u_shadowAtlas (directional/spot, projective) and
    //     u_shadowPointAtlas (point cube faces); removes the u_shadowCubeMap* samplers
    //     and keeps each atlas within the GPU max texture size.
    // v10: 2D lighting — mesh USE_LIGHT2D/USE_SHADOWS_2D variants (u_fs_lighting2d,
    //      u_normalTexture on unlit, v_position guard) and the shadow2d 1D polar pass.
    // v11: uniform-driven shadow PCF (ShadowQuality) — USE_SHADOWS_PCF variant removed,
    //      shadows.glsl/lighting2d.glsl loop by radius from cameraDir.w / atlasInfo.w.
    // v12: fixed game resolution — new blit fullscreen shader (blit.frag).
    // v13: box-projected local reflection probes in the mesh IBL path.
    // v14: G-buffer (SSR) terrain fix — gbuffer.vert no longer declares a_texcoord1
    //      for terrain (generates the base-tile UV in-shader), so the terrain G-buffer
    //      pipeline's vertex layout is continuous and passes sokol validation.
    // v15: skinning capacity increased to 128 bones; every skinned shader uniform block changed.
    // v16: projected spotlight masks add spotUp_maskAspect[MAX_LIGHTS] to u_fs_lighting
    //      and add the shared u_spotMaskAtlas sampler to punctual mesh variants,
    //      using the engine's native texture Y orientation.
    // v17: projective shadows use a depth-only atlas with a comparison sampler.
    // v18: vec3 vertex colors on meshes — v_color is promoted to vec4 under instancing.
    // v19: Vulkan shadow atlas lookups and screen-space depth/texture orientation fixes,
    //      later widened from IS_VULKAN to every non-GL backend (Metal, D3D11) across
    //      the depth, G-buffer and fullscreen passes: same shaders, so still v19.
    // v20: dynamically sized skinning uses storage buffers, or an unfilterable bone
    //      texture with a nonfiltering sampler on GL/GLES.
    // v21: backend-specific storage-buffer bindings are preserved in reflection data.
    // v22: composite.frag samples the scene color and reflection in destination space
    //      (params.w), so SSR can composite to the swapchain in exported builds.
    // v23: explicit SSR LOD sampling prevents D3D11 loop-unrolling failures.
    // v24: instanced distance fade (USE_INSTANCE_FADE) scales each instance toward its own
    //      base in the mesh and depth vertex shaders, from the new u_vs_fade block. Terrain
    //      detail layers blend by height, project triplanar on steep ground and break their
    //      tiling with distance, from three new terrain varyings.
    // v25: skinned normals and tangents are transformed as directions (mat3), no longer
    //      as points, so posed bone translations stop flattening the lighting.
    // v26: shadow PCF loop bounds are clamped on every backend, not just HLSL, so a
    //      bad uniform can no longer produce an unbounded loop (a GPU hang).
    // v27: per-layer PBR terrain (HAS_TERRAIN_PBR) — layer maps are slices of the detail
    //      array driven by u_fs_terrainLayers, in the color pass and the G-buffer alike.
    // v28: paired bilinear shadow PCF taps; depth.vert takes a prebuilt mvpMatrix and
    //      gl_Position is invariant, so a depth prepass matches the color pass; instanced
    //      normals use the cofactor matrix; backfaces flip the shading normal; leaf
    //      transmission adds foliageTransmission to u_fs_pbrParams.
    // v29: sprite and tilemap UVs (HAS_TEXTURERECT) use centroid interpolation, so MSAA
    //      edge pixels no longer sample outside their atlas rect.
    // v30: unified lighting — per-object lights blocks (u_vs/u_fs_objectLights, includes/
    //      lights.glsl) with vertex-light ('Vtl') and capacity-tier ('Lc4', 'L16') bits; Lit
    //      reads the camera and fog from the frame block (u_fs_fog and u_fs_lighting are gone
    //      from the built-in shader; forks declaring u_fs_lighting are still filled).
    // v31: one light unit for every shader (includes/lights.glsl): vertexDiffuse and the
    //      folded SH lights carry Lambert's 1/PI, so Lit no longer divides them itself.
    // v32: the lights block packs 4 vectors per light (the vertex stage declares a prefix),
    //      Lit reads its own camera block (u_fs_litFrame), unlit fog is u_fs_fog again, the
    //      depth programs take the sprite texture rect ('Txr'), library variants a blend define.
    // v33: merge of the HTML UI work (its v30 and v31 on the html-ui branch): HTML UI gradient
    //      and post-process shaders (HTMLUI_GRADIENT, HTMLUI_POST), HTML UI effects (UI_EFFECT),
    //      library effects named by their custom-shader id; the ShaderType values of HTMLUI ...
    //      UI_EFFECT moved after BLOOM.
    // v34: Stochastic Tiling (includes/stochastic.glsl) — u_fs_terrainLayers gains
    //      stochastic[MAX_TERRAIN_LAYERS], read by the PBR terrain path of the color pass and
    //      the G-buffer; library shaders gain the STOCHASTIC feature.
    //      Also .sdat format version 2 (the key holds 64 property bits). Version 1 files would be
    //      rejected and rebuilt anyway; the new directory keeps an older editor's cache apart.
    // v35: Stochastic Tiling of the built-in Lit material — MESH bit 5 'Sto' (USE_STOCHASTIC_TILING:
    //      includes/pbr.glsl, u_fs_pbrParams gains a vec4), DEPTH bit 14 and GBUFFER bit 10 'Sto'
    //      (u_fs_depthMaterial / u_fs_gbufferMaterial gain one); lighting2d.glsl's normal map.
    //      Also supersedes v34, which two branches each bumped to (the terrain/library Stochastic
    //      Tiling and the 64-bit shader keys): a v34 cache from either could be trusted by the other.
    // v36: terrain triplanar: the vertex normal from central differences, the three projections
    //      weighted by abs(normal)^8 (the slope smoothstep and the two-way side split are gone),
    //      projections under 2% skipped.
    // v37: mesh.frag skips the shadow lookup of a surface facing away from the light (unless
    //      foliage transmission reads it); ssr.frag fades out rough surfaces (SSR_MAX_ROUGHNESS)
    //      and skips their march; probe_filter (PROBE_FILTER) prefilters runtime reflection probe
    //      captures into roughness mips, each source level downsampled from the one above.
    //      library:water's REFLECTION, CAUSTICS, UNDERSIDE and BUILTIN_TEXTURES features, its
    //      distant detail fade and new parameters (u_fs_customParams grows); includes/lights.glsl
    //      gains ggxSpecular; the library post-process shader library:underwater.
    // v38: lattice deformer (includes/lattice.glsl): HAS_LATTICE paths in mesh/depth/gbuffer.vert
    //      and the surface includes (MESH bit 32 'Ltc', DEPTH bit 15, GBUFFER bits 11-13), the
    //      tangent of includes/surface_vs.glsl transformed before the hooks. (Builds of the lattice
    //      branch used v34/v35 with 'Ltc' at MESH bit 5, then v37 before this v37 of main.)
    // (oe-port) v34: (the S12 branch only: mesh bit 5 as a height fog bit; S10 has it as 'Shq')
    // (oe-port) v35: atmosphere (S12): the fogged mesh variants (HAS_FOG) carry the height fog
    //      (HAS_HEIGHT_FOG, u_fs_heightFog), so every cached 'Fog' variant is stale; the
    //      atmosphere pass types after S10's SHADOW_CACHE.
    // S16 side, v36: HTML UI rendering cost (S16): htmlui.vert/.frag gain the document position and
    //      u_fs_htmluiClip (analytic clip, R8 coverage textures); HTMLUI_PRIM (instanced SDF
    //      boxes, borders, shadows and glyph quads) appended after the atmosphere types.
    // v37: S15's PASS_PREVIEW before HTMLUI_PRIM (the merge of both); the HTML UI's rounded-rect
    //      distance and border colours changed (S16).
    // v38: HTMLUI_PRIM samples four textures (u_htmluiTexture0-3, u_fs_htmluiSlots, S16).
    // S16 v39: HTMLUI_PRIM gradients take their origin and tile period (S16-F5).
    // v39: upstream (v34-v38 above) and oe-port's S10/S12 (its v34/v35) together: MESH bit 5 is
    //      'Sto' and S10's high shadow filters moved to bit 33 ('Shq', USE_SHADOWS_HQ); the
    //      fogged variants carry the height fog. Either side's v34-v38 cache holds other variants.
    // v40: S11's built-in Lit (lobes, hex tiling, Lit Quality: pbr.glsl, mesh.frag, terrain) with
    //      upstream's Stochastic Tiling in the same Lit and terrain sources and block layout.
    // v41: S16's HTMLUI_PRIM and htmlui changes (its v36-v39) with upstream's and S11's (v39, v40).
    // v42: S20's PARTICLES before HTMLUI_PRIM (the merge of both): a v41 cache numbers them otherwise.
    // (upstream) v39: vertex animation (includes/vat.glsl): vatEvaluate() and the vatPosition/
    //      vatNormal/... wrappers in every vertex entry point, HAS_VAT / HAS_VAT_BONES (MESH bits
    //      33/34, DEPTH 16/17, GBUFFER 14/15).
    // v43: upstream's vertex animation (its v39) with oe-port's v42: MESH 'Vat'/'Vab' are bits 34/35
    //      (S10's 'Shq' keeps 33), DEPTH 16/17, GBUFFER 14/15; an upstream cache's MESH bit 33 is 'Vat'.
    // (upstream) v40: sprite normal maps: the 2D tangent frame (includes/tangent_frame.glsl, mesh.vert,
    //      includes/surface_vs.glsl: OpenGL green, mirroring), the Light 2D normal response
    //      (includes/lighting2d.glsl: LIGHT2D_NORMAL_RESPONSE, getNormal2DAt, mesh.frag's
    //      LIGHT2D_SURFACE_NORMALS), @require normalmap (library/sprite.frag) and stochastic.glsl.
    // v44: upstream's sprite normal maps and shader globals (u_vs_globals / u_fs_globals, its v40)
    //      with oe-port's v43: either side's v40-v43 cache holds other variants.
    // v45: S57's lighting (MESH 'Til', 'Cap', 'Cts', 'Oit' at bits 36-39, the Hair lobe's
    //      PBR_HAIR in u_fs_pbrParams, includes/lights.glsl's tiled and occlusion forms).
    // v46: S54's LOD cross-fade: MESH 'Lfd' (includes/lod_fade.glsl in mesh.vert and mesh.frag), and
    //      every instance matrix read with its first column's w as 0.
    // v47: S55's automatic GPU instancing: mesh and depth programs take their object block from
    //      includes/object_params.glsl / object_depth_params.glsl, 'Aui' (MESH 41, DEPTH 18).
    // v48: the fade variant reads its LOD fade in main (objectLodFade() a macro): under v47 a
    //      skinned fade variant's uniform block slots differed from its base's.
    // v49: SSR reflection sources (doriax-catchup ssr-probes): GBUFFER 'Occ'/'Uv2', the G-buffer's
    //      fourth attachment (occlusion) and its reflection source code; the composite rebuilds
    //      probes and the ambient, reads SSAO and the fog; ssr.frag reads the source.
    // v50: render-correctness: includes/instance_fade.glsl (the near fade, the G-buffer's 'Ifd'),
    //      lighting2d's and the SSAO's explicit-LOD lookups, mesh.frag's shadow derivatives before
    //      the light loop (lightShadowD), pbr.glsl's derived (flat) normals turned toward the eye.
    // v51: water-reflections: library:water on the scene clock (sceneTime), GERSTNER,
    //      PLANAR_REFLECTION (includes/planar_reflection.glsl), its shadow lookup with the smooth
    //      normal and shadowed caustics; @role in water.frag and underwater.frag.
    // v52: S91's library:particles DISTORTION (scene colour, u_distortionTexture, distortionStrength
    //      in u_fs_customParams).
    return App::getUserCacheBaseDir() / "jasperengine" / "shaders" / "v52";
}

void editor::App::pushTabNotificationStyle(){
    // Unselected tab with unseen content: a muted take on the selection blue.
    ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.17f, 0.27f, 0.37f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed,   ImVec4(0.17f, 0.27f, 0.37f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,  ImVec4(0.23f, 0.35f, 0.47f, 1.00f));
}

void editor::App::popTabNotificationStyle(){
    ImGui::PopStyleColor(3);
}

editor::Project* editor::App::getProject(){
    return &project;
}

const editor::Project* editor::App::getProject() const{
    return &project;
}

editor::InspectorWindow* editor::App::getInspectorWindow() const{
    return inspectorWindow;
}

editor::CodeEditor* editor::App::getCodeEditor() const{
    return codeEditor;
}

editor::ImageViewerWindow* editor::App::getImageViewerWindow() const{
    return imageViewerWindow;
}

editor::ProjectWindow* editor::App::getProjectWindow() const{
    return projectWindow;
}

bool editor::App::popSaveDialogQueueItem() {
    if (saveDialogQueue.empty()) {
        Out::warning("Attempted to pop an empty save dialog queue.");
        return false;
    }

    saveDialogQueue.pop();
    return true;
}

editor::AnimationWindow* editor::App::getAnimationWindow() const{
    return animationWindow;
}

editor::JobsMemoryWindow* editor::App::getJobsMemoryWindow() const{
    return jobsMemoryWindow;
}

editor::PoseAnimationWindow* editor::App::getPoseAnimationWindow() const{
    return poseAnimationWindow;
}

editor::AvatarWindow* editor::App::getAvatarWindow() const{
    return avatarWindow;
}

void editor::App::openAvatarWindow(uint32_t sceneId, Entity entity){
    std::string error;
    // the bound model keeps its edits; another one asks about unsaved edits first
    if (!avatarWindow->requestOpen(sceneId, entity, &error)){
        Out::warning("Configure Avatar: %s", error.c_str());
    }
}

editor::TerrainEditWindow* editor::App::getTerrainEditWindow() const{
    return terrainEditWindow;
}

editor::TextureSynthesisWindow* editor::App::getTextureSynthesisWindow() const{
    return textureSynthesisWindow;
}

editor::VertexAnimationBakerWindow* editor::App::getVertexAnimationBakerWindow() const{
    return vertexAnimationBakerWindow;
}

void editor::App::openVertexAnimationBaker(const std::filesystem::path& path, bool bakeNow){
    std::string error;
    const std::string file = FileUtils::pathToUtf8(path);
    if (!vertexAnimationBakerWindow->setSourcePath(file, error)) {
        registerAlert("Vertex Animation Baker", "Can't open " + FileUtils::pathToUtf8(path.filename()) + ": " + error);
        return;
    }
    vertexAnimationBakerWindow->open();
    if (bakeNow && !vertexAnimationBakerWindow->bake(true, error)) {
        registerAlert("Vertex Animation Baker", "Can't bake " + FileUtils::pathToUtf8(path.filename()) + ": " + error);
    }
}

editor::PhysicsPlacementWindow* editor::App::getPhysicsPlacementWindow() const{
    return physicsPlacementWindow;
}

editor::SpriteNormalsWindow* editor::App::getSpriteNormalsWindow() const{
    return spriteNormalsWindow;
}

editor::SpriteNormalsWatcher* editor::App::getSpriteNormalsWatcher() const{
    return spriteNormalsWatcher;
}

void editor::App::openSpriteNormals(const std::vector<std::filesystem::path>& images, bool editRecipe){
    std::string error;
    if (editRecipe && !images.empty()) {
        if (!spriteNormalsWindow->openRecipe(images[0], error)) {
            registerAlert("Sprite Normal Map", "Can't open " + FileUtils::pathToUtf8(images[0].filename()) + ": " + error);
        }
        return;
    }
    if (!spriteNormalsWindow->openSources(images, error)) {
        registerAlert("Sprite Normal Map", "Can't open " + (images.size() == 1 ? FileUtils::pathToUtf8(images[0].filename()) : std::string("the images")) + ": " + error);
    }
}

void editor::App::openSpriteNormalsForEntity(uint32_t sceneId, Entity entity, unsigned submesh, bool now){
    std::string error;
    if (!spriteNormalsWindow->openForEntity(sceneId, static_cast<Entity>(entity), submesh, now, error)) {
        SceneProject* sp = project.getScene(sceneId);
        const std::string name = sp && sp->scene && sp->scene->isEntityCreated(static_cast<Entity>(entity)) ? sp->scene->getEntityName(static_cast<Entity>(entity)) : "the GameObject";
        registerAlert("Sprite Normal Map", "Can't open " + name + ": " + error);
    }
}

editor::MainToolbar* editor::App::getMainToolbar() const{
    return mainToolbar;
}

void editor::App::openBuildReport(const std::filesystem::path& report){
    buildReportWindow->open(report);
}

void editor::App::openMeshAudit(){
    showEditorWindow(meshAuditWindow);
}

void editor::App::openConsole(){
    showEditorWindow(consoleWindow);
}

void editor::App::openAudioMixerWindow(const std::filesystem::path& mixer){
    audioMixerWindow->open(mixer);
}

void editor::App::openParticleEffect(const std::string& assetPath){
    particleEffectWindow->openEffect(assetPath);
}

void editor::App::openParticleEffectSubtree(uint32_t sceneId, uint32_t root){
    particleEffectWindow->openSubtree(sceneId, (Entity)root);
}

void editor::App::openTextureSynthesis(const std::filesystem::path& image, bool editRecipe){
    if (!editRecipe) {
        textureSynthesisWindow->openWithExample(image);
        return;
    }
    std::string error;
    if (!textureSynthesisWindow->openRecipe(image, error)) {
        registerAlert("Edit Synthesis", "Can't restore the synthesis of " + image.filename().string() + ": " + error);
    }
}

editor::SystemsWindow* editor::App::getSystemsWindow() const{
    return systemsWindow;
}

editor::HierarchyWindow* editor::App::getHierarchyWindow() const{
    return hierarchyWindow;
}

editor::SceneWindow* editor::App::getSceneWindow() const{
    return sceneWindow;
}

void editor::App::openPreferences(const std::string& category){
    editorSettingsWindow.open(&project, category);
}

editor::ConsoleWindow* editor::App::getConsoleWindow() const{
    return consoleWindow;
}

editor::PlayInput* editor::App::getPlayInput() const{
    return playInput;
}

editor::GameWindow* editor::App::getGameWindow() const{
    return gameWindow;
}

// ---- window layouts (S25 R15-R19, layout/LayoutManager.h)

namespace {

    // The dockable panels a layout opens or closes, by window name (the preset's `panels:`)
    template <typename F>
    void forEachPanel(F&& visit, editor::HierarchyWindow* hierarchy, editor::InspectorWindow* inspector,
                      editor::ProjectWindow* projectWindow, editor::ConsoleWindow* console, editor::AnimationWindow* animation,
                      editor::TerrainEditWindow* terrain, editor::mcp::McpActivityWindow* mcpActivity, editor::SystemsWindow* systems,
                      editor::MissingAssetsWindow* missing, editor::AssetReferencesWindow* references, editor::JobsMemoryWindow* jobs,
                      editor::ProfilerWindow* profiler, editor::FrameDebuggerWindow* frameDebugger, editor::UndoHistoryWindow* undo,
                      editor::PlayModeChangesWindow* playChanges, editor::GameWindow* game, editor::BookmarksWindow* bookmarks,
                      editor::TextureSynthesisWindow* textureSynthesis, editor::PhysicsPlacementWindow* physicsPlacement,
                      editor::PoseAnimationWindow* poseAnimation, editor::VertexAnimationBakerWindow* vertexAnimationBaker, editor::AnimatorWindow* animator,
                      editor::AudioMixerWindow* audioMixer, editor::ParticleEffectWindow* particleEffect,
                      editor::GraphEditorWindow* graphEditor) {
        visit(editor::GameWindow::WINDOW_NAME, game);
        visit(editor::InspectorWindow::WINDOW_NAME, inspector);
        visit(editor::HierarchyWindow::WINDOW_NAME, hierarchy);
        visit(editor::ProjectWindow::WINDOW_NAME, projectWindow);
        visit(editor::ConsoleWindow::WINDOW_NAME, console);
        visit(editor::AnimationWindow::WINDOW_NAME, animation);
        visit(editor::TerrainEditWindow::WINDOW_NAME, terrain);
        visit(editor::mcp::McpActivityWindow::WINDOW_NAME, mcpActivity);
        visit(editor::SystemsWindow::WINDOW_NAME, systems);
        visit(editor::MissingAssetsWindow::WINDOW_NAME, missing);
        visit(editor::AssetReferencesWindow::WINDOW_NAME, references);
        visit(editor::JobsMemoryWindow::WINDOW_NAME, jobs);
        visit(editor::ProfilerWindow::WINDOW_NAME, profiler);
        visit(editor::FrameDebuggerWindow::WINDOW_NAME, frameDebugger);
        visit(editor::UndoHistoryWindow::WINDOW_NAME, undo);
        visit(editor::PlayModeChangesWindow::WINDOW_NAME, playChanges);
        visit(editor::BookmarksWindow::WINDOW_NAME, bookmarks);
        visit(editor::TextureSynthesisWindow::WINDOW_NAME, textureSynthesis);
        visit(editor::PhysicsPlacementWindow::WINDOW_NAME, physicsPlacement);
        visit(editor::PoseAnimationWindow::WINDOW_NAME, poseAnimation);
        visit(editor::AnimatorWindow::WINDOW_NAME, animator);
        visit(editor::VertexAnimationBakerWindow::WINDOW_NAME, vertexAnimationBaker);
        visit(editor::AudioMixerWindow::WINDOW_NAME, audioMixer);
        visit(editor::ParticleEffectWindow::WINDOW_NAME, particleEffect);
        visit(editor::GraphEditorWindow::WINDOW_NAME, graphEditor);
    }

}

std::string editor::App::focusedWindowName() const{
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || !context->NavWindow) return {};
    const ImGuiWindow* window = context->NavWindow->RootWindow ? context->NavWindow->RootWindow : context->NavWindow;
    return window->Name ? std::string(window->Name) : std::string();
}

std::map<std::string, bool> editor::App::panelStates() const{
    std::map<std::string, bool> panels;
    forEachPanel([&panels](const char* name, auto* window) { panels[name] = window->isOpen(); },
                 hierarchyWindow, inspectorWindow, projectWindow, consoleWindow, animationWindow, terrainEditWindow,
                 mcpActivityWindow, systemsWindow, missingAssetsWindow, assetReferencesWindow, jobsMemoryWindow, profilerWindow,
                 frameDebuggerWindow, undoHistoryWindow, playModeChangesWindow, gameWindow, bookmarksWindow,
                 textureSynthesisWindow, physicsPlacementWindow, poseAnimationWindow, vertexAnimationBakerWindow, animatorWindow,
                 audioMixerWindow, particleEffectWindow, graphEditorWindow);
    return panels;
}

void editor::App::applyPanelStates(const std::map<std::string, bool>& panels){
    // panels the layout does not name keep their state (a window newer than the layout)
    forEachPanel([&panels](const char* name, auto* window) {
        auto it = panels.find(name);
        if (it != panels.end()) window->setOpen(it->second);
    }, hierarchyWindow, inspectorWindow, projectWindow, consoleWindow, animationWindow, terrainEditWindow,
       mcpActivityWindow, systemsWindow, missingAssetsWindow, assetReferencesWindow, jobsMemoryWindow, profilerWindow,
       frameDebuggerWindow, undoHistoryWindow, playModeChangesWindow, gameWindow, bookmarksWindow,
                 textureSynthesisWindow, physicsPlacementWindow, poseAnimationWindow, vertexAnimationBakerWindow, animatorWindow,
                 audioMixerWindow, particleEffectWindow, graphEditorWindow);
}

void editor::App::applyDefaultLayout(){
    hierarchyWindow->setOpen(true);
    inspectorWindow->setOpen(true);
    projectWindow->setOpen(true);
    consoleWindow->setOpen(true);
    animationWindow->setOpen(true);
    poseAnimationWindow->setOpen(false);
    animatorWindow->setOpen(false);
    terrainEditWindow->setOpen(false);
    textureSynthesisWindow->setOpen(false);
    audioMixerWindow->setOpen(false);
    particleEffectWindow->setOpen(false);
    graphEditorWindow->setOpen(false);
    vertexAnimationBakerWindow->setOpen(false);
    physicsPlacementWindow->setOpen(false);
    mcpActivityWindow->setOpen(false);
    systemsWindow->setOpen(false);
    gameWindow->setOpen(true);
    buildDockspace(true);
}

void editor::App::setupLayouts(){
    LayoutManager::Host host;
    host.panelStates = [this]() { return panelStates(); };
    host.applyPanels = [this](const std::map<std::string, bool>& panels) { applyPanelStates(panels); };
    host.applyDefault = [this]() { applyDefaultLayout(); };
    host.afterIniLoaded = [this](float uiScale) {
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) return false;
        // sizes in the ini are pixels at the layout's scale (rescaleRestoredLayout)
        layoutUiScale = uiScale;
        layoutScaleApplied = false;
        rescaleRestoredLayout();
        // the central node, then the open documents (not in a layout) into it
        forceDockTabs = false;
        dock_id_middle_top = getCentralDockId();
        dockProjectTabs();
        ImGui::DockBuilderFinish(dockspace_id);
        return true;
    };
    host.currentUiScale = [this]() { return layoutScaleApplied ? layoutUiScale : mainViewportScale(); };
    host.projectPath = [this]() {
        return (project.isEditorTempProject() || project.getProjectPath().empty()) ? std::filesystem::path() : project.getProjectPath();
    };
    host.promptUnsaved = [this](const std::string& title, const std::string& message, std::function<void()> onSave,
                                std::function<void()> onDontSave) {
        registerThreeButtonAlert(title, message, std::move(onSave), std::move(onDontSave));
        alert.yesLabel = "Save";
        alert.noLabel = "Don't Save";
    };
    host.confirm = [this](const std::string& title, const std::string& message, std::function<void()> onYes) {
        registerConfirmAlert(title, message, std::move(onYes));
    };
    host.alert = [this](const std::string& title, const std::string& message) { registerAlert(title, message); };
    layouts.setHost(std::move(host));
}

void editor::App::resetLayout(){
    layouts.requestSwitch(LayoutPresets::DEFAULT_NAME);
}

void editor::App::processNextSaveDialog() {
    if (saveDialogInProgress || saveDialogQueue.empty() || sceneSaveDialog.isOpen() || projectSaveDialog.isOpen()) {
        return;
    }

    SaveDialogQueueItem item = saveDialogQueue.front();
    // every way out answers the caller once: true only when the file was written
    std::function<void(bool)> completion = item.callback;
    auto finish = [this, completion](bool saved) {
        saveDialogInProgress = false;
        popSaveDialogQueueItem();
        if (completion) completion(saved);
        processNextSaveDialog();
    };

    if (item.type == SaveDialogType::Scene) {
        const uint32_t sceneId = item.sceneId;
        SceneProject* sceneProject = project.getScene(sceneId);
        if (!sceneProject) {
            popSaveDialogQueueItem();
            if (completion) completion(false);
            processNextSaveDialog();
            return;
        }

        std::string defaultName = sceneProject->name + ".scene";
        fs::path initialDirectory = project.getProjectPath();
        if (projectWindow) {
            fs::path resourcesPath = projectWindow->getCurrentPath();
            if (!resourcesPath.empty()) {
                initialDirectory = resourcesPath;
            }
        }

        saveDialogInProgress = true;
        sceneSaveDialog.open(
            project.getProjectPath(),
            initialDirectory,
            defaultName,
            [this, sceneId, finish](const fs::path& fullPath) {
                // the scene takes the path only once the file is written (saveSceneFile)
                if (!project.getScene(sceneId)) {
                    finish(false);
                    return;
                }
                project.saveSceneToPathAsync(sceneId, fullPath, finish);
            },
            [finish]() { finish(false); },
            [this, sceneId](const fs::path& fullPath) { return project.sceneSavePathProblem(sceneId, fullPath); }
        );
    }
    else if (item.type == SaveDialogType::Project) {
        std::string defaultName = project.getName();
        if (defaultName.empty()) {
            defaultName = "MyProject";
        }

        saveDialogInProgress = true;
        projectSaveDialog.open(
            defaultName,
            [this, finish](const std::string& projectName, const fs::path& projectPath) {
                if (!projectName.empty()) {
                    project.setName(projectName);
                }
                const bool saved = project.saveProjectToPath(projectPath);
                if (!saved) registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");
                finish(saved);
            },
            [finish]() { finish(false); }
        );
    }
}

void editor::App::cancelSaveDialogs() {
    std::queue<SaveDialogQueueItem> pending;
    std::swap(pending, saveDialogQueue);
    sceneSaveDialog.abandon();
    projectSaveDialog.abandon();
    saveDialogInProgress = false;
    // answered after the queue is empty, so none of them opens another dialog
    while (!pending.empty()) {
        std::function<void(bool)> callback = std::move(pending.front().callback);
        pending.pop();
        if (callback) callback(false);
    }
}

void editor::App::processMainThreadTasks() {
    if (projectLoading) return;

    // FIFO, one failing task neither drops the ones behind it (an MCP call waiting for its
    // turn would never be answered) nor takes the editor down (Scheduler logs it)
    Scheduler& tasks = scheduler();
    tasks.processPosted();
    const auto now = std::chrono::steady_clock::now();
    if (lastSchedulerTime != std::chrono::steady_clock::time_point{}) {
        tasks.advance(TimeDomain::Real, std::chrono::duration<double>(now - lastSchedulerTime).count());
    }
    lastSchedulerTime = now;
    tasks.process(TimeDomain::Real);
    tasks.advance(TimeDomain::Frames, 1.0);
    tasks.process(TimeDomain::Frames);
}

bool editor::App::isMainThread() const {
    return std::this_thread::get_id() == mainThreadId;
}

void editor::App::initializeSettings() {
    AppSettings::initialize(mcp::mcpLaunchOptions().stdio);

    // Next to settings.yaml, where a user can find it to report a crash.
    const std::filesystem::path logFile = AppSettings::getConfigDirectory() / "editor.log";
    Out::setLogFile(logFile);
    // report folders, the session marker and the hang watchdog (docs/crash-diagnostics.md)
    EditorDiagnostics::install(AppSettings::getConfigDirectory());

    applyPanelVisibilitySettings();
}

void editor::App::applyPanelVisibilitySettings() {
    const PanelVisibilitySettings visibility = AppSettings::getPanelVisibility();
    hierarchyWindow->setOpen(visibility.hierarchy);
    inspectorWindow->setOpen(visibility.inspector);
    projectWindow->setOpen(visibility.project);
    consoleWindow->setOpen(visibility.console);
    animationWindow->setOpen(visibility.animation);
    poseAnimationWindow->setOpen(visibility.poseAnimation);
    animatorWindow->setOpen(visibility.animator);
    terrainEditWindow->setOpen(visibility.terrain);
    textureSynthesisWindow->setOpen(visibility.textureSynthesis);
    audioMixerWindow->setOpen(visibility.audioMixer);
    particleEffectWindow->setOpen(visibility.particleEffect);
    soundBrowserWindow->setOpen(visibility.soundBrowser);
    graphEditorWindow->setOpen(visibility.graphEditor);
    spriteNormalsWindow->setOpen(visibility.spriteNormals);
    vertexAnimationBakerWindow->setOpen(visibility.vertexAnimationBaker);
    physicsPlacementWindow->setOpen(visibility.physicsPlacement);
    mcpActivityWindow->setOpen(visibility.mcp);
    gameWindow->setOpen(visibility.game);
    undoHistoryWindow->setOpen(visibility.undoHistory);
}

void editor::App::persistPanelVisibilitySettings() {
    PanelVisibilitySettings visibility;
    visibility.hierarchy = hierarchyWindow->isOpen();
    visibility.inspector = inspectorWindow->isOpen();
    visibility.project = projectWindow->isOpen();
    visibility.console = consoleWindow->isOpen();
    visibility.animation = animationWindow->isOpen();
    visibility.poseAnimation = poseAnimationWindow->isOpen();
    visibility.animator = animatorWindow->isOpen();
    visibility.terrain = terrainEditWindow->isOpen();
    visibility.textureSynthesis = textureSynthesisWindow->isOpen();
    visibility.audioMixer = audioMixerWindow->isOpen();
    visibility.particleEffect = particleEffectWindow->isOpen();
    visibility.soundBrowser = soundBrowserWindow->isOpen();
    visibility.graphEditor = graphEditorWindow->isOpen();
    visibility.spriteNormals = spriteNormalsWindow->isOpen();
    visibility.vertexAnimationBaker = vertexAnimationBakerWindow->isOpen();
    visibility.physicsPlacement = physicsPlacementWindow->isOpen();
    visibility.mcp = mcpActivityWindow->isOpen();
    visibility.game = gameWindow->isOpen();
    visibility.undoHistory = undoHistoryWindow->isOpen();

    if (visibility != AppSettings::getPanelVisibility()) {
        AppSettings::setPanelVisibility(visibility);
    }
}

int editor::App::getInitialWindowWidth(float uiScale) const {
    return scaleSavedWindowSize(AppSettings::getWindowWidth(), uiScale);
}

int editor::App::getInitialWindowHeight(float uiScale) const {
    return scaleSavedWindowSize(AppSettings::getWindowHeight(), uiScale);
}

bool editor::App::getInitialWindowMaximized() const {
    return AppSettings::getIsMaximized();
}

void editor::App::saveWindowSettings(int width, int height, bool maximized, float uiScale) {
    AppSettings::setWindowWidth(width);
    AppSettings::setWindowHeight(height);
    AppSettings::setIsMaximized(maximized);
    AppSettings::setWindowUiScale(sanitizeUiScale(uiScale));
    // The ini is written at the scale the layout has held all session, not the
    // monitor scale above, which is read fresh and may have changed since.
    if (layoutScaleApplied) {
        AppSettings::setLayoutUiScale(sanitizeLayoutScale(layoutUiScale));
    }
    AppSettings::saveSettings();
}

void editor::App::exit() {
    if (projectLoading) {
        Backend::closeWindow();
        return;
    }
    // import settings edited and not applied (S53 R17): asked first, the close follows Apply or Revert
    if (ImporterPrompt::guard([this]() { exit(); })) return;

    // Check if any modal popup is currently open (including ComponentAddDialog, ScriptCreateDialog, etc.)
    ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal();
    if (modal != nullptr) {
        // A modal is open - close it first
        ImGui::CloseCurrentPopup();
        return;  // Don't proceed with exit yet, user needs to click close again
    }

    // An open save dialog is cancelled first (with the ones queued behind it)
    if (sceneSaveDialog.isOpen() || projectSaveDialog.isOpen()) {
        cancelSaveDialogs();
        return;
    }

    if (project.isAnySceneSaving()) {
        registerAlert("Saving Scene", "Wait for the current scene save to finish before exiting.");
        return;
    }

    // the Sprite Normal Map window's unsaved paint strokes (not in any file): asked first, like its close
    if (spriteNormalsWindow && spriteNormalsWindow->hasUnsavedPaint()) {
        registerConfirmAlert("Discard Paint Changes?",
                             "The Sprite Normal Map window has paint strokes that are not saved into the normal map.\n\nDiscard them and exit?",
                             [this]() {
                                 spriteNormalsWindow->discardPaint();
                                 exit();
                             });
        return;
    }

    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        registerThreeButtonAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before exiting?",
            [this]() {
                saveAllAndProject([this]() {
                    closeWindow();
                });
            },
            [this]() {
                // No callback - just exit without saving
                closeWindow();
            },
            []() {
                // Cancel callback - do nothing, just close the dialog
            }
        );
    } else {
        // No unsaved changes, proceed with exit
        closeWindow();
    }
}

void editor::App::closeWindow(){
    // A moved camera, a different selected scene and a changed terrain brush live
    // only in the model until this runs, and nothing else asks for it. Also flushes
    // a tab reorder still sitting in captureTabOrder()'s debounce.
    if (!benchmarkEnabled) {
        project.saveWorkspaceFile();
        tabsOrderDirty = false;
    }

    // Stop all playing scenes before shutdown to properly cleanup script instances
    for (auto& sceneProject : project.getScenes()) {
        if (sceneProject.playState == ScenePlayState::PLAYING || 
            sceneProject.playState == ScenePlayState::PAUSED ||
            sceneProject.playState == ScenePlayState::LOADING) {
            project.stop(sceneProject.id);
        }
    }

    project.waitForPlaySessionToFinish();

    project.clearTrash();

    editor::ShaderBuilder::requestShutdown();
    Backend::closeWindow();
}
