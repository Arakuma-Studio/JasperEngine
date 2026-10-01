// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "App.h"

#include "imgui_internal.h"
#include "Platform.h"
#include "Backend.h"
#include "Engine.h"
#include "render/SystemRender.h"

#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "util/EntityClipboard.h"

#include "util/CrashHandler.h"
#include "util/FileDialogs.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"

#include "EditorVersion.h"
#include "Out.h"
#include "AppSettings.h"
#include "Theme.h"
#include "resources/fonts/fa-solid-900_ttf.h"
#include "resources/fonts/jetbrains-mono-regular_ttf.h"
#include "resources/fonts/roboto-latin-bold_ttf.h"
//#include "recources/fonts/roboto-v20-latin-regular_ttf.h"
#include "util/DefaultFont.h"
#include "util/DefaultFontArabic.h"

#include "shader/ShaderBuilder.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"

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

// The dockspace lives on the main viewport, so that is the scale it has to match.
float mainViewportScale() {
    return sanitizeUiScale(ImGui::GetMainViewport()->DpiScale);
}

// SizeRef against a central node is read as an absolute size, so it has to grow
// with the text; siblings split by ratio stay proportional under the same factor.
void scaleDockChildren(ImGuiDockNode* node, float ratio) {
    for (ImGuiDockNode* child : node->ChildNodes) {
        if (!child) continue;
        child->Size = ImVec2(child->Size.x * ratio, child->Size.y * ratio);
        child->SizeRef = ImVec2(child->SizeRef.x * ratio, child->SizeRef.y * ratio);
        scaleDockChildren(child, ratio);
    }
}

}

ImFont* editor::App::codeFont = nullptr;
ImFont* editor::App::boldFont = nullptr;

editor::App::App(){
    mainThreadId = std::this_thread::get_id();
    inspectorWindow = new InspectorWindow(&project);
    consoleWindow = new ConsoleWindow();
    playInput = new PlayInput(&project);
    sceneWindow = new SceneWindow(&project);
    gameWindow = new GameWindow(&project, playInput);
    mainToolbar = new MainToolbar(&project);
    inspectorWindow->setSceneWindow(sceneWindow);
    hierarchyWindow = new HierarchyWindow(&project, sceneWindow);
    codeEditor = new CodeEditor(&project);
    imageViewerWindow = new ImageViewerWindow(&project);
    projectWindow = new ProjectWindow(&project, codeEditor, imageViewerWindow);
    loadingWindow = new LoadingWindow();
    animationWindow = new AnimationWindow(&project);
    terrainEditWindow = new TerrainEditWindow(&project);
    aiChatWindow = new AiChatWindow(&project, projectWindow);

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

void editor::App::saveAllFunc(std::function<void(bool)> callback){
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
            registerProjectSaveDialog([callback]() { if (callback) callback(true); });
        } else {
            const bool saved = project.saveProject(true);
            if (!saved) registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");
            if (callback) callback(saved);
        }
    });
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

    auto openProject = [this]() {
        #ifdef _WIN32
        const char* homeDir = std::getenv("USERPROFILE");
        #else
        const char* homeDir = std::getenv("HOME");
        #endif
        const std::string selectedDir = FileDialogs::openFileDialog(
            homeDir ? homeDir : "", FILE_DIALOG_ALL, true);
        if (selectedDir.empty()) return;

        const std::filesystem::path projectDir(selectedDir);
        if (!std::filesystem::exists(projectDir / "project.yaml")) {
            registerAlert("Error", "The selected directory is not a valid project. No project.yaml file found!");
            return;
        }

        requestProjectChange([this, projectDir]() { project.loadProject(projectDir); });
    };
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

enum class AppMenuCommand : uint32_t {
    NewProject = 1,
    NewScene3D,
    NewScene2D,
    NewSceneUI,
    OpenProject,
    OpenRecentProject,
    ClearRecentProjects,
    SaveProjectAs,
    Save,
    SaveAll,
    ExportProject,
    EditorSettings,
    Exit,
    Undo,
    Redo,
    SelectAll,
    CutSelection,
    CopySelection,
    PasteSelection,
    PasteAsChild,
    DuplicateSelection,
    DeleteSelection,
    RenameSelection,
    FrameSelected,
    ToggleHierarchy,
    ToggleInspector,
    ToggleProject,
    ToggleConsole,
    ToggleAnimation,
    ToggleTerrain,
    ToggleAiChat,
    FocusScene,
    FocusGame,
    StepFrame,
    ToggleDetachableWindows,
    ResetLayout,
    ProjectSettings,
    ProjectScenes,
    ProjectPrefabs,
    ClearTrash,
    TogglePlay,
    TogglePause,
    StopScene,
    RemoveScene,
    CreateGameObject,     // payload: index into HierarchyWindow::getCreateMenuEntries()
    CreateEmptyChild,
    AddComponent,
    AssetsCreateFolder,
    AssetsCreateLuaScript,
    AssetsCreateCppScript,
    AssetsShowInFileManager,
    AssetsOpen,
    AssetsRename,
    AssetsDelete,
    AssetsImport,
    AssetsRefresh,
    About,
    Documentation,
    KeyboardShortcuts,
    ReportIssue
};

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

editor::PlatformMenuItem menuShortcut(AppMenuCommand command,
                                      std::string label,
                                      std::string shortcut,
                                      bool enabled = true) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label), enabled);
    item.shortcut = std::move(shortcut);
    return item;
}

editor::PlatformMenuItem menuToggle(AppMenuCommand command,
                                    std::string label,
                                    bool checked,
                                    std::string shortcut = {},
                                    bool enabled = true) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label), enabled);
    item.checked = checked;
    item.shortcut = std::move(shortcut);
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
// in the same order and grouping the Hierarchy's own menus use.
std::vector<editor::PlatformMenuItem> gameObjectMenuItems(editor::SceneProject* scene,
                                                          bool canCreate,
                                                          bool canCreateChild) {
    using editor::HierarchyWindow;
    using editor::CreateMenuEntry;
    const auto& entries = HierarchyWindow::getCreateMenuEntries();
    auto offered = [scene](const CreateMenuEntry& entry) {
        return !scene || HierarchyWindow::isCreateEntryAvailable(entry, scene->sceneType);
    };
    auto entryItem = [&](size_t index) {
        const CreateMenuEntry& entry = entries[index];
        editor::PlatformMenuItem item = menuPayloadCommand(AppMenuCommand::CreateGameObject,
            entry.label, std::to_string(index), canCreate);
        if (entry.shortcut) item.shortcut = entry.shortcut;
        return item;
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
                items.push_back(menuShortcut(AppMenuCommand::CreateEmptyChild, "Create Empty Child",
                                             "Alt+Shift+N", canCreateChild));
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

editor::PlatformMenuModel editor::App::buildMenuModel(){
    SceneProject* selectedScene = project.getSelectedScene();
    uint32_t selectedSceneId = project.getSelectedSceneId();
    bool hasSelectedScene = selectedScene != nullptr;
    bool isProjectBusy = project.isAnyScenePlaying();
    bool canRemove = hasSelectedScene && !isProjectBusy && project.getScenes().size() > 1;
    const bool textInput = ImGui::GetIO().WantTextInput;

    bool canSave = false;
    if (!saveDialogInProgress) {
        if (project.isTempProject()) {
            canSave = !isProjectBusy;
        } else if (lastFocusedWindow == LastFocusedWindow::Code) {
            canSave = codeEditor->hasLastFocusedUnsavedChanges();
        } else {
            canSave = !isProjectBusy && project.hasSelectedSceneUnsavedChanges();
        }
    }
    // Project settings are not tracked by the scene and script dirty flags.
    bool canSaveAll = !isProjectBusy && !saveDialogInProgress;

    bool canUndo = false;
    bool canRedo = false;
    if (!textInput) {
        if (lastFocusedWindow == LastFocusedWindow::Resources) {
            canUndo = project.getProjectCommandHistory()->canUndo();
            canRedo = project.getProjectCommandHistory()->canRedo();
        } else if (lastFocusedWindow == LastFocusedWindow::Code) {
            canUndo = codeEditor->canUndoLastFocused();
            canRedo = codeEditor->canRedoLastFocused();
        } else if (lastFocusedWindow != LastFocusedWindow::AI && hasSelectedScene) {
            canUndo = CommandHandle::get(selectedSceneId)->canUndo();
            canRedo = CommandHandle::get(selectedSceneId)->canRedo();
        }
    }

    // Single-key shortcuts (F, F2) must not steal keys from text fields, the code
    // editor or a running game (native menus own their key equivalents)
    const bool sceneContext = !textInput && !isProjectBusy && lastFocusedWindow == LastFocusedWindow::AnySceneWindow;
    const size_t selectedCount = hasSelectedScene ? project.getSelectedEntities(selectedSceneId).size() : 0;
    // the selection may be of a scene layered over the selected one (Scene view picks)
    const bool canFrame = sceneContext && hasSelectedScene && sceneWindow->hasFrameableSelection(selectedSceneId);
    const bool canRename = sceneContext && selectedCount == 1;
    const bool canCreate = hasSelectedScene && selectedScene->scene;
    const bool canCreateChild = canCreate && hierarchyWindow->getSelectedParentForCreate() != NULL_ENTITY;
    const bool canAddComponent = hasSelectedScene && selectedCount > 0 && !isProjectBusy;

    const bool assetSelected = projectWindow->hasAssetSelection();
    const bool singleAsset = projectWindow->hasSingleAssetSelection();

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
        recentItems.push_back(menuCommand(AppMenuCommand::ClearRecentProjects,
                                          "Clear Recent Projects"));
    }

    auto newSceneItems = [&]() {
        return std::vector<PlatformMenuItem>{
            menuCommand(AppMenuCommand::NewScene3D, "3D"),
            menuCommand(AppMenuCommand::NewScene2D, "2D"),
            menuCommand(AppMenuCommand::NewSceneUI, "UI")
        };
    };

    PlatformMenuModel menu;
    menu.menus.push_back(menuSubmenu("File", {
        menuSubmenu("New Scene", newSceneItems(), !isProjectBusy),
        menuShortcut(AppMenuCommand::Save, "Save", "Ctrl+S", canSave),
        menuShortcut(AppMenuCommand::SaveAll, "Save All", "Ctrl+Shift+S", canSaveAll),
        menuCommand(AppMenuCommand::RemoveScene, "Remove Scene from Project", canRemove),
        menuSeparator(),
        menuCommand(AppMenuCommand::NewProject, "New Project...", !isProjectBusy),
        menuShortcut(AppMenuCommand::OpenProject, "Open Project...", "Ctrl+O", !isProjectBusy),
        menuSubmenu("Recent Projects", std::move(recentItems), !isProjectBusy),
        menuCommand(AppMenuCommand::SaveProjectAs, "Save Project As...", canSaveAll),
        menuSeparator(),
        menuShortcut(AppMenuCommand::ExportProject, "Build Settings...", "Ctrl+Shift+B"),
        menuSeparator(),
        menuCommand(AppMenuCommand::Exit, "Exit")
    }));

    menu.menus.push_back(menuSubmenu("Edit", {
        menuShortcut(AppMenuCommand::Undo, "Undo", "Ctrl+Z", canUndo),
#if defined(__APPLE__)
        menuShortcut(AppMenuCommand::Redo, "Redo", "Ctrl+Shift+Z", canRedo),
#else
        menuShortcut(AppMenuCommand::Redo, "Redo", "Ctrl+Y", canRedo),
#endif
        menuSeparator(),
        menuShortcut(AppMenuCommand::SelectAll, "Select All", "Ctrl+A", canSelectAll()),
        menuSeparator(),
        menuShortcut(AppMenuCommand::CutSelection, "Cut", "Ctrl+X", canCutSelection()),
        menuShortcut(AppMenuCommand::CopySelection, "Copy", "Ctrl+C", canCopySelection()),
        menuShortcut(AppMenuCommand::PasteSelection, "Paste", "Ctrl+V", canPasteSelection(false)),
        menuShortcut(AppMenuCommand::PasteAsChild, "Paste As Child", "Ctrl+Shift+V", canPasteSelection(true)),
        menuSeparator(),
        menuShortcut(AppMenuCommand::DuplicateSelection, "Duplicate", "Ctrl+D", canEditSelection(true)),
        menuShortcut(AppMenuCommand::RenameSelection, "Rename", "F2", canRename),
        menuShortcut(AppMenuCommand::DeleteSelection, "Delete", "Delete", canEditSelection(false)),
        menuSeparator(),
        menuShortcut(AppMenuCommand::FrameSelected, "Frame Selected", "F", canFrame),
        menuSeparator(),
        menuToggle(AppMenuCommand::TogglePlay, "Play", mainToolbar->isInPlayMode(), "Ctrl+P", mainToolbar->canTogglePlay()),
        menuToggle(AppMenuCommand::TogglePause, "Pause", mainToolbar->isPaused(), "Ctrl+Shift+P", mainToolbar->canPause()),
        menuShortcut(AppMenuCommand::StepFrame, "Step", "Ctrl+Alt+P", mainToolbar->canStep()),
        menuCommand(AppMenuCommand::StopScene, "Stop", mainToolbar->isInPlayMode()),
        menuSeparator(),
        menuCommand(AppMenuCommand::ProjectSettings, "Project Settings..."),
        menuCommand(AppMenuCommand::EditorSettings, "Preferences...")
    }));

    menu.menus.push_back(menuSubmenu("Assets", {
        menuSubmenu("Create", {
            menuCommand(AppMenuCommand::AssetsCreateFolder, "Folder"),
            menuSeparator(),
            menuCommand(AppMenuCommand::AssetsCreateLuaScript, "Lua Script"),
            menuCommand(AppMenuCommand::AssetsCreateCppScript, "C++ Script"),
            menuSeparator(),
            menuSubmenu("Scene", newSceneItems(), !isProjectBusy)
        }),
        menuCommand(AppMenuCommand::AssetsShowInFileManager, "Show in File Manager"),
        menuCommand(AppMenuCommand::AssetsOpen, "Open", singleAsset),
        menuCommand(AppMenuCommand::AssetsRename, "Rename", singleAsset),
        menuCommand(AppMenuCommand::AssetsDelete, "Delete", assetSelected),
        menuSeparator(),
        menuCommand(AppMenuCommand::AssetsImport, "Import New Asset..."),
        menuCommand(AppMenuCommand::AssetsRefresh, "Refresh"),
        menuSeparator(),
        menuCommand(AppMenuCommand::ProjectScenes, "Scenes in Build...", !isProjectBusy),
        menuCommand(AppMenuCommand::ProjectPrefabs, "Prefabs..."),
        menuCommand(AppMenuCommand::ClearTrash, "Empty Trash...", !isProjectBusy)
    }));

    menu.menus.push_back(menuSubmenu("GameObject",
        gameObjectMenuItems(selectedScene, canCreate, canCreateChild)));

    menu.menus.push_back(menuSubmenu("Component", {
        menuShortcut(AppMenuCommand::AddComponent, "Add...", "Ctrl+Shift+A", canAddComponent)
    }));

    menu.menus.push_back(menuSubmenu("Window", {
        menuSubmenu("General", {
            menuShortcut(AppMenuCommand::FocusScene, "Scene", "Ctrl+1", hasSelectedScene),
            menuShortcut(AppMenuCommand::FocusGame, "Game", "Ctrl+2"),
            menuShortcut(AppMenuCommand::ToggleInspector, windowMenuLabel(InspectorWindow::WINDOW_NAME), "Ctrl+3"),
            menuShortcut(AppMenuCommand::ToggleHierarchy, windowMenuLabel(HierarchyWindow::WINDOW_NAME), "Ctrl+4"),
            menuShortcut(AppMenuCommand::ToggleProject, windowMenuLabel(ProjectWindow::WINDOW_NAME), "Ctrl+5"),
            menuShortcut(AppMenuCommand::ToggleConsole, windowMenuLabel(ConsoleWindow::WINDOW_NAME), "Ctrl+Shift+C")
        }),
        menuSubmenu("Animation", {
            menuShortcut(AppMenuCommand::ToggleAnimation, windowMenuLabel(AnimationWindow::WINDOW_NAME), "Ctrl+6")
        }),
        menuCommand(AppMenuCommand::ToggleTerrain, windowMenuLabel(TerrainEditWindow::WINDOW_NAME)),
        menuSubmenu("AI", {
            menuCommand(AppMenuCommand::ToggleAiChat, windowMenuLabel(AiChatWindow::WINDOW_NAME))
        }),
        menuSeparator(),
        menuSubmenu("Layouts", {
            menuCommand(AppMenuCommand::ResetLayout, "Reset Layout")
        }),
        menuToggle(AppMenuCommand::ToggleDetachableWindows, "Detachable Windows",
                   AppSettings::getMultiViewportEnabled())
    }));

    menu.menus.push_back(menuSubmenu("Help", {
        menuCommand(AppMenuCommand::About, "About JasperEngine"),
        menuSeparator(),
        menuShortcut(AppMenuCommand::Documentation, "Documentation", "F1"),
        menuCommand(AppMenuCommand::KeyboardShortcuts, "Keyboard Shortcuts"),
        menuCommand(AppMenuCommand::ReportIssue, "Report an Issue...")
    }));
    return menu;
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
    switch (action) {
        case AppMenuCommand::NewProject: {
            if (project.isAnyScenePlaying()) return;
            std::string projectName = "MyJasperEngineProject";
            auto startFreshProject = [this, projectName]() {
                requestProjectChange([this, projectName]() {
                    if (project.createTempProject(projectName, true)) aiChatWindow->startNewChat();
                });
            };
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
            registerProjectSaveDialog([](){});
            break;
        case AppMenuCommand::Save:
            saveFunc();
            break;
        case AppMenuCommand::SaveAll:
            saveAllFunc();
            break;
        case AppMenuCommand::ExportProject:
            exportWindow.open(&project);
            break;
        case AppMenuCommand::Exit:
            exit();
            break;
        case AppMenuCommand::Undo:
            if (lastFocusedWindow == LastFocusedWindow::Resources) {
                project.getProjectCommandHistory()->undo();
                projectWindow->refreshCurrentDirectory();
            } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                codeEditor->undoLastFocused();
            } else if (lastFocusedWindow != LastFocusedWindow::AI && project.getSelectedScene()) {
                CommandHandle::get(project.getSelectedSceneId())->undo();
            }
            break;
        case AppMenuCommand::Redo:
            if (lastFocusedWindow == LastFocusedWindow::Resources) {
                project.getProjectCommandHistory()->redo();
                projectWindow->refreshCurrentDirectory();
            } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                codeEditor->redoLastFocused();
            } else if (lastFocusedWindow != LastFocusedWindow::AI && project.getSelectedScene()) {
                CommandHandle::get(project.getSelectedSceneId())->redo();
            }
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
        case AppMenuCommand::ToggleTerrain:
            showEditorWindow(terrainEditWindow);
            break;
        case AppMenuCommand::ToggleAiChat:
            showEditorWindow(aiChatWindow);
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
            hierarchyWindow->setOpen(true);
            inspectorWindow->setOpen(true);
            projectWindow->setOpen(true);
            consoleWindow->setOpen(true);
            animationWindow->setOpen(true);
            terrainEditWindow->setOpen(false);
            aiChatWindow->setOpen(true);
            gameWindow->setOpen(true);
            buildDockspace(true);
            break;
        case AppMenuCommand::ProjectSettings:
            projectSettingsWindow.open(&project);
            break;
        case AppMenuCommand::EditorSettings:
            editorSettingsWindow.open(&project);
            break;
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
        case AppMenuCommand::AssetsCreateFolder:
            projectWindow->assetsCreateFolder();
            break;
        case AppMenuCommand::AssetsCreateLuaScript:
            projectWindow->assetsCreateScript(true);
            break;
        case AppMenuCommand::AssetsCreateCppScript:
            projectWindow->assetsCreateScript(false);
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
        case AppMenuCommand::AssetsRefresh:
            projectWindow->assetsRefresh();
            break;
        case AppMenuCommand::About:
            aboutWindow.open();
            break;
        case AppMenuCommand::DuplicateSelection:
            duplicateSelection();
            break;
        case AppMenuCommand::DeleteSelection:
            deleteSelection();
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
            registerAlert("Keyboard Shortcuts",
                "FILE AND EDIT\n"
                "Ctrl+S    Save current scene or script\n"
                "Ctrl+Shift+S    Save all scenes, scripts, and project\n"
                "Ctrl+O    Open project\n"
                "Ctrl+Shift+B    Build Settings\n"
                "Ctrl+Z    Undo\n"
                "Ctrl+Y / Ctrl+Shift+Z    Redo\n"
                "Ctrl+A    Select All\n"
                "Ctrl+X / Ctrl+C / Ctrl+V    Cut / Copy / Paste GameObjects\n"
                "Ctrl+Shift+V    Paste As Child\n"
                "Ctrl+D    Duplicate\n"
                "F2    Rename\n"
                "Delete / Backspace    Delete\n\n"
                "GAMEOBJECT AND COMPONENT\n"
                "Ctrl+Shift+N    Create Empty\n"
                "Alt+Shift+N    Create Empty Child\n"
                "Ctrl+Shift+A    Add Component\n\n"
                "PLAY MODE\n"
                "Ctrl+P    Play / Stop (F5 plays, F7 stops)\n"
                "Ctrl+Shift+P    Pause / Resume (F6 pauses)\n\n"
                "WINDOWS\n"
                "Ctrl+1 Scene   Ctrl+3 Inspector   Ctrl+4 Hierarchy\n"
                "Ctrl+5 Project   Ctrl+6 Animation   Ctrl+Shift+C Console\n\n"
                "SCENE VIEW\n"
                "W / E / R    Move / Rotate / Scale\n"
                "T    Toggle local / global transforms\n"
                "F / Home    Frame selected / all (3D)\n\n"
                "F1    Documentation",
                "On macOS, menu shortcuts use Command instead of Ctrl.");
            break;
    }
}

void editor::App::showImGuiMenuItems(const std::vector<PlatformMenuItem>& items){
    for (const PlatformMenuItem& item : items) {
        if (item.type == PlatformMenuItemType::Separator) {
            ImGui::Separator();
        } else if (item.type == PlatformMenuItemType::Submenu) {
            if (ImGui::BeginMenu(item.label.c_str(), item.enabled)) {
                showImGuiMenuItems(item.children);
                ImGui::EndMenu();
            }
        } else if (ImGui::MenuItem(
                       item.label.c_str(),
                       item.shortcut.empty() ? nullptr : item.shortcut.c_str(),
                       item.checked, item.enabled)) {
            executeMenuCommand(item.command);
        }
    }
}

void editor::App::showImGuiMenu(const PlatformMenuModel& menu){
    if (ImGui::BeginMainMenuBar()) {
        for (const PlatformMenuItem& topLevel : menu.menus) {
            if (ImGui::BeginMenu(topLevel.label.c_str(), topLevel.enabled)) {
                // Paste reflects what another editor instance or application copied
                if (topLevel.label == "Edit" && ImGui::IsWindowAppearing()) {
                    EntityClipboard::refresh();
                }
                showImGuiMenuItems(topLevel.children);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMainMenuBar();
    }
}

void editor::App::showMenu(){
    PlatformMenuModel menu = buildMenuModel();
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
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    // Reserve space from the viewport work area so dockspace doesn't overlap
    viewport->WorkSize.y -= footerHeight;
}

void editor::App::showAlert(){
    if (alert.needShow) {
        ImGui::OpenPopup((alert.title + "##AlertModal").c_str());

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_Modal;

        if (ImGui::BeginPopupModal((alert.title + "##AlertModal").c_str(), nullptr, flags)) {
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
                // For three-button alerts, show Yes, No and Cancel buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 370; // Width for three buttons
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
    const float savedScale = sanitizeUiScale(layoutUiScale);
    layoutUiScale = currentScale;
    layoutScaleApplied = true;

    const float ratio = currentScale / savedScale;
    if (ratio > 0.99f && ratio < 1.01f) return;

    // The root node is pinned to the viewport, so only its descendants move.
    scaleDockChildren(root, ratio);
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
        return std::min(std::max(fraction * available, minFontSizes * ImGui::GetFontSize()), available * 0.45f);
    };
    const float rightWidth = share(0.22f, viewport.x, 16);
    const float leftWidth = share(0.18f, viewport.x, 12);
    const float bottomHeight = share(0.30f, viewport.y, 10);

    // Inspector on the right, full height; AI Chat and Terrain behind it.
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.0f, &dock_id_right, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(rightWidth, viewport.y));
    ImGui::DockBuilderSetNodeSize(dock_id_middle, ImVec2(viewport.x - rightWidth, viewport.y));
    ImGui::DockBuilderDockWindow(InspectorWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(AiChatWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(TerrainEditWindow::WINDOW_NAME, dock_id_right);

    // Project, Console and Animation across the bottom, under Hierarchy + Scene.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Down, 0.0f, &dock_id_middle_bottom, &dock_id_left);
    ImGui::DockBuilderSetNodeSize(dock_id_middle_bottom, ImVec2(viewport.x - rightWidth, bottomHeight));
    ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(viewport.x - rightWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderDockWindow(ProjectWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(ConsoleWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(AnimationWindow::WINDOW_NAME, dock_id_middle_bottom);

    // Hierarchy top-left; the Scene tabs and Game view fill the centre.
    ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Left, 0.0f, &dock_id_left_top, &dock_id_middle_top);
    ImGui::DockBuilderSetNodeSize(dock_id_left_top, ImVec2(leftWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderSetNodeSize(dock_id_middle_top, ImVec2(viewport.x - rightWidth - leftWidth, viewport.y - bottomHeight));
    ImGui::DockBuilderDockWindow(HierarchyWindow::WINDOW_NAME, dock_id_left_top);
    ImGui::DockBuilderDockWindow("Game", dock_id_middle_top);

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
    stampPanelDockOrder(AiChatWindow::WINDOW_NAME, 1);
    stampPanelDockOrder(TerrainEditWindow::WINDOW_NAME, 2);
    stampPanelDockOrder(ProjectWindow::WINDOW_NAME, 0);
    stampPanelDockOrder(ConsoleWindow::WINDOW_NAME, 1);
    stampPanelDockOrder(AnimationWindow::WINDOW_NAME, 2);

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

void editor::App::showStyleEditor(){
    ImGui::Begin("Dear ImGui Style Editor", nullptr);
    {
        // Get the current IO object to access display size
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 windowSize = ImGui::GetWindowSize();

        // Calculate the centered position at the top
        float windowX = (io.DisplaySize.x - windowSize.x - 200) / 2;
        float windowY = 0.0f; // Top of the screen

        // Set the window position
        ImGui::SetWindowPos(ImVec2(windowX, windowY), ImGuiCond_Once);
        ImGui::SetWindowCollapsed(true, ImGuiCond_Once);

        ImGui::ShowStyleEditor();
    }
    ImGui::End();
}

ImFont* editor::App::getCodeFont() {
    return codeFont ? codeFont : ImGui::GetIO().Fonts->Fonts[0];
}

ImFont* editor::App::getBoldFont() {
    return boldFont ? boldFont : ImGui::GetFont();
}

void editor::App::setup() {
    mainThreadId = std::this_thread::get_id();

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

    // Persist the docking layout next to settings.yaml at a stable absolute
    // path, so it survives restarts regardless of the working directory or
    // build type. (initializeSettings() above has already set the config dir.)
    // Versioned so a layout saved before the Unity-style default (and window
    // titles) is left behind instead of restored: bump when the default changes.
    layoutIniPath = (AppSettings::getConfigDirectory() / "editor_layout_v2.ini").string();
    io.IniFilename = layoutIniPath.c_str();

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

    io.FontDefault = font1;

    io.ConfigDragClickToInputText = true;

    //io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

    //ImGui::StyleColorsDark();
    Theme::apply();

    // Fonts follow each viewport's monitor DPI. Style paddings are global, so
    // App::show() re-scales them from the main viewport when that DPI changes.
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    float dpiScale = 1.0f;
    if (ImGui::GetPlatformIO().Monitors.Size > 0) {
        dpiScale = ImGui::GetPlatformIO().Monitors[0].DpiScale;
    }
    Theme::applyDpiScale(dpiScale);
}

bool editor::App::canEditSelection(bool duplicate) {
    // Enabled items keep their native key equivalent, stealing it from a focused text field.
    if (ImGui::GetIO().WantTextInput) return false;
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
    // A child is selected through its parent, as in the Hierarchy
    project.replaceSelectedEntities(scene->id, Project::getTopLevelEntities(scene->scene, scene->entities));
}

void editor::App::copySelection(bool cut) {
    if (!(cut ? canCutSelection() : canCopySelection())) return;
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

    EntityClipboard::update();

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
        if (projectWindow->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::Resources;
        } else if (codeEditor->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::Code;
        } else if (aiChatWindow->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::AI;
        }else{
            lastFocusedWindow = LastFocusedWindow::AnySceneWindow;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    bool isUndo = !io.WantTextInput && (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift);
    bool isRedo = !io.WantTextInput && ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyShift));

    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!popupOpen) {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (ImGui::GetIO().KeyShift) {
                // Save scenes, scripts, and project configuration together.
                saveAllFunc();
            } else {
                saveFunc();
            }
        }

        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
            // CTRL+O opens a project
            openProjectFunc();
        }

        // Play mode (Ctrl+P, Ctrl+Shift+P, Ctrl+Alt+P, F5/F6/F7) and tool shortcuts
        mainToolbar->handleShortcuts(!codeEditor->isFocused() && !aiChatWindow->isFocused());
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::Documentation), {}});

        // Unity menu shortcuts. Not while typing: text fields and the code editor
        // use modifier+letter chords of their own.
        if (!io.WantTextInput && !codeEditor->isFocused()) {
            auto chord = [&io](bool ctrl, bool shift, bool alt, ImGuiKey key) {
                return io.KeyCtrl == ctrl && io.KeyShift == shift && io.KeyAlt == alt && ImGui::IsKeyPressed(key, false);
            };
            auto run = [this](AppMenuCommand menuCommand, std::string payload = {}) {
                executeMenuCommand({static_cast<uint32_t>(menuCommand), std::move(payload)});
            };
            if (chord(true, true, false, ImGuiKey_B)) run(AppMenuCommand::ExportProject);
            if (chord(true, true, false, ImGuiKey_N)) {
                const auto& entries = HierarchyWindow::getCreateMenuEntries();
                for (size_t i = 0; i < entries.size(); i++) {
                    if (entries[i].type == EntityCreationType::GAMEOBJECT) {
                        run(AppMenuCommand::CreateGameObject, std::to_string(i));
                        break;
                    }
                }
            }
            if (chord(false, true, true, ImGuiKey_N)) run(AppMenuCommand::CreateEmptyChild);
            if (chord(true, true, false, ImGuiKey_A)) run(AppMenuCommand::AddComponent);
            if (chord(true, false, false, ImGuiKey_1)) run(AppMenuCommand::FocusScene);
            if (chord(true, false, false, ImGuiKey_2)) run(AppMenuCommand::FocusGame);
            if (chord(true, false, false, ImGuiKey_3)) run(AppMenuCommand::ToggleInspector);
            if (chord(true, false, false, ImGuiKey_4)) run(AppMenuCommand::ToggleHierarchy);
            if (chord(true, false, false, ImGuiKey_5)) run(AppMenuCommand::ToggleProject);
            if (chord(true, false, false, ImGuiKey_6)) run(AppMenuCommand::ToggleAnimation);
            if (chord(true, true, false, ImGuiKey_C)) run(AppMenuCommand::ToggleConsole);
        }
    }

    if (isDroppedExternalPaths) {
        isDroppedExternalPaths = false;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            std::vector<char> payload = Util::encodeStringsPayload(droppedExternalPaths);
            ImGui::SetDragDropPayload("external_files", payload.data(), payload.size());
            ImGui::EndDragDropSource();
        }
    }

    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused()){
        // space to keys events
    }

    if (!popupOpen && !projectWindow->isFocused() && !codeEditor->isFocused() && !aiChatWindow->isFocused()){
        uint32_t sceneId = project.getSelectedSceneId();

        // Update the Undo and Redo button logic:
        if (isUndo) {
            CommandHandle::get(sceneId)->undo();
        }
        if (isRedo) {
            CommandHandle::get(sceneId)->redo();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) deleteSelection();

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelection();

        // Clipboard and Select All: only for the Hierarchy and the Scene view, where the keys
        // are not text operations (the Console and the Project window have their own)
        if (!io.WantTextInput && io.KeyCtrl && !io.KeyAlt && (hierarchyWindow->isFocused() || sceneWindow->isFocused())) {
            auto run = [this](AppMenuCommand menuCommand) {
                executeMenuCommand({static_cast<uint32_t>(menuCommand), {}});
            };
            if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) run(AppMenuCommand::SelectAll);
            if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_X, false)) run(AppMenuCommand::CutSelection);
            if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) run(AppMenuCommand::CopySelection);
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) run(io.KeyShift ? AppMenuCommand::PasteAsChild : AppMenuCommand::PasteSelection);
        }

        // F2 renames inline in the Hierarchy when it has focus, else in the Inspector
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            if (!(hierarchyWindow->isFocused() && hierarchyWindow->renameSelected())) inspectorWindow->focusNameInput();
        }

        // Frame Selected (Edit menu): the Scene view handles F itself while it has focus
        if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
            hierarchyWindow->isFocused() && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::FrameSelected), {}});
        }
    }

    if (!popupOpen && projectWindow->isFocused()) {
        if (isUndo) {
            project.getProjectCommandHistory()->undo();
            projectWindow->refreshCurrentDirectory();
        }
        if (isRedo) {
            project.getProjectCommandHistory()->redo();
            projectWindow->refreshCurrentDirectory();
        }
    }

    dockspace_id = ImGui::GetID("MyDockspace");

    showMenu();
    mainToolbar->show();
    showFooter();

    isInitialized = true;

    // Before DockSpaceOverViewport() below lays the restored sizes out.
    rescaleRestoredLayout();

    if (dockspaceNeedsRebuild || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDockspace();
        dockspaceNeedsRebuild = false;
    }

    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    #ifdef SHOW_STYLE_WINDOW
    showStyleEditor();
    #endif

    showAlert();

    sceneSaveDialog.show();
    projectSaveDialog.show();
    exportWindow.show();
    if (exportWindow.consumePlayerSettingsRequest()) projectSettingsWindow.open(&project, "Player");
    if (inspectorWindow->consumeTagsAndLayersRequest()) projectSettingsWindow.open(&project, "Tags and Layers");
    editorSettingsWindow.show();
    aboutWindow.show();
    projectSettingsWindow.show();
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
    terrainEditWindow->show();
    inspectorWindow->show();
    aiChatWindow->show();
    codeEditor->show();
    gameWindow->show();
    sceneWindow->show();
    Backend::setGameCursorInSceneRect(gameWindow->isGameCursorInView());

    loadingWindow->show();

    // Keep the persisted tab list in sync with the live tab order so a user's
    // drag-reordering of scene/code tabs is saved (and restored on next launch).
    captureTabOrder();

    // Closing a panel through its title-bar button changes the same state as the
    // View menu, so mirror the live state after all panels have rendered.
    persistPanelVisibilitySettings();
}

void editor::App::setLoadingPump(std::function<void(bool)> pump) {
    loadingPump = std::move(pump);
}

void editor::App::reportLoadingProgress(const std::string& status) {
    if (!isMainThread() || !projectLoading || pumpingLoading) return;
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
                benchmarkProject = value;
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
        } else if (arg == "--depth-prepass") {
            if (const char* value = requireValue(i, "--depth-prepass"))
                benchmarkDepthPrepass = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--name") {
            if (const char* value = requireValue(i, "--name"))
                benchmarkName = value;
        } else if (arg == "--out") {
            if (const char* value = requireValue(i, "--out"))
                benchmarkOut = value;
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
    fs::path scenePath = benchmarkScene;
    if (scenePath.is_relative())
        scenePath = project.getProjectPath() / scenePath;

    SceneProject* match = nullptr;
    for (SceneProject& sceneProject : project.getScenes()) {
        const bool wanted = benchmarkScene.empty()
            ? sceneProject.id == project.getStartSceneId()
            : (sceneProject.name == benchmarkScene || sceneProject.filepath.filename() == benchmarkScene ||
               project.getProjectPath() / sceneProject.filepath == scenePath);
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
    Engine::systemInit(argc, argv, new editor::Platform(&project));

    Engine::pauseGameEvents(true);

    // Any edit/undo/redo flags its scene for redraw, so the loop can idle otherwise.
    CommandHistory::onSceneModified = [this](size_t sceneId, bool structural){
        if (SceneProject* sceneProject = project.getScene((uint32_t)sceneId)){
            sceneProject->needUpdateRender = true;
            // Only structural commands, so a gizmo drag doesn't rebuild the tree each frame.
            if (structural){
                sceneProject->structureVersion++;
            }
        }
    };

    ShaderPool::setShaderBuilder([this](jasperengine::ShaderKey shaderKey) -> jasperengine::ShaderBuildResult {
        static jasperengine::editor::ShaderBuilder builder;  // Make static to reuse
        return builder.buildShader(shaderKey, &project);
    });

    Engine::setAsyncLoading(true);
}

void editor::App::engineViewLoaded(){
    reportLoadingProgress("Initializing graphics...");
    Engine::systemViewLoaded();
}

void editor::App::loadStartupProject() {
    lastLoadingFrame = {};
    reportLoadingProgress("Loading project...");

    // a failed benchmark still runs one frame: the main loop exits on benchmarkExit, and
    // closing before the first frame would leave the render queue waiting on shutdown
    if (benchmarkEnabled) {
        if (benchmarkExit)
            return;

        fs::path projectPath = benchmarkProject;
        if (fs::is_regular_file(projectPath) && projectPath.filename() == "project.yaml")
            projectPath = projectPath.parent_path();

        if (!project.loadProject(projectPath, false)) {
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

    std::filesystem::path lastProjectPath = AppSettings::getLastProjectPath();

    if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
        if (project.loadProject(lastProjectPath)) {
            Out::info("Loaded last opened project: \"%s\"", lastProjectPath.string().c_str());
        } else {
            project.createTempProject("MyJasperEngineProject");
        }
    } else {
        project.createTempProject("MyJasperEngineProject");
    }

    projectLoading = false;
    requestRedraw();
}

void editor::App::requestProjectChange(std::function<void()> change) {
    if (projectLoading || pendingProjectChange) return;
    pendingProjectChange = std::move(change);
    requestRedraw();
}

void editor::App::processProjectChange() {
    if (projectLoading || !pendingProjectChange) return;
    auto change = std::move(pendingProjectChange);
    pendingProjectChange = nullptr;
    if (project.isAnyScenePlaying()) return;

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

void editor::App::engineRender(){
    if (projectLoading) {
        SystemRender::executeQueue();
        return;
    }

    processMainThreadTasks();
    project.refreshLinkedMaterials();
    project.refreshAssetDatabase();
    project.updateGeneratedSources();
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
        if (benchmarkEnabled) active = true;
        // A running play session must advance and redraw its simulation every frame.
        if (sp.playState == ScenePlayState::PLAYING) active = true;
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
            if (sp.sceneRender->isPreviewCameraActive()) active = true;
            gaugeAnimating = sp.sceneRender->getUILayer()->isCameraGaugeAnimating();
            if (thumbnailsPending) active = true;
            // Preview scenes (shape, material, direction) only draw inside systemDraw().
            if (Engine::hasScenesToExecuteOnce()) active = true;
        }

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

    for (auto& sceneProject : project.getScenes()) {
        if (!sceneProject.opened) continue;
        if (!sceneProject.scene || !sceneProject.sceneRender) continue;
        if (sceneProject.playState == ScenePlayState::SAVING || sceneProject.playState == ScenePlayState::LOADING || sceneProject.playState == ScenePlayState::CANCELLING) continue;

        const bool isSelected = (sceneProject.id == selectedSceneId);

        // The running game renders into the Game view's framebuffer, shown or not, and
        // the Scene view is a second, draw-only view of the same frame
        if (gameWindow->hostsScene(sceneProject.id)) {
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
                    childScene->scene->getSystem<RenderSystem>()->update(0);
                    childScene->needUpdateRender = false;
                }
            }

            // to avoid delay when move objects with gizmo
            sceneRender->updateRenderSystem();

            //TODO: avoid calling every frame
            project.updateSceneRender(sceneProject.id);

            // A paused game advances one frame here when Step was pressed
            const bool steppingFrame = project.beginStepFrame(sceneProject.id);

            Engine::systemDraw();

            if (steppingFrame) {
                project.endStepFrame(sceneProject.id);
            }

            projectWindow->processMaterialThumbnails();
            projectWindow->processModelThumbnails();

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

    bool viewChanged = false;
    if (lastActivatedScene != sceneProject.id || !lastActivatedGame){
        // Collected before activateGame(): it drops every engine layer, and a dropped
        // layer no longer counts as running, so this would come back empty.
        std::vector<Scene*> runtimeLayers = project.getRunningRuntimeLayers(sceneProject.id);
        sceneRender->activateGame(gameTarget);

        for (Scene* runtimeLayer : runtimeLayers) {
            Engine::addSceneLayer(runtimeLayer);
        }

        lastActivatedScene = sceneProject.id;
        lastActivatedGame = true;
        viewChanged = true;
    }

    if (pendingResizeScene == sceneProject.id) {
        viewChanged = true;
        pendingResizeScene = NULL_PROJECT_SCENE;
    }

    const bool framebufferMismatch = !gameTarget->isCreated()
        || gameTarget->getWidth() != static_cast<unsigned int>(gameWidth)
        || gameTarget->getHeight() != static_cast<unsigned int>(gameHeight);

    // the Engine's view is the game's: the Scene view never changes it
    if (Platform::setSizes(gameWidth, gameHeight) || viewChanged || framebufferMismatch){
        Engine::systemViewChanged();
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
        sceneRender->drawSceneView(sceneWidth, sceneHeight, project.getRunningRuntimeLayers(sceneProject.id), true);
    }

    projectWindow->processMaterialThumbnails();
    projectWindow->processModelThumbnails();

    renderedSceneThisFrame = true;
    sceneProject.needUpdateRender = false;
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
            childScene->scene->update(Engine::getDeltatime());
            childScene->needUpdateRender = false;
        }
    }
    sceneRender->setChildSceneLayers(childLayers);

    sceneRender->setOverlayScale(sceneWindow->getOverlayScale(sceneProject.id));
    if (sceneRender->getViewWidth() != width || sceneRender->getViewHeight() != height) {
        sceneRender->updateSize(width, height);
    }
    project.updateSceneRender(sceneProject.id);
    sceneRender->updateRenderSystem();
    sceneProject.scene->update(Engine::getDeltatime());

    sceneRender->drawSceneView(width, height, childLayers, false);

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
        target->create();
    }

    int canvasWidth = 0;
    int canvasHeight = 0;
    Rect viewRect;
    Engine::computeViewLayout(width, height, (int)project.getCanvasWidth(), (int)project.getCanvasHeight(), project.getScalingMode(),
                              canvasWidth, canvasHeight, viewRect);

    std::vector<Engine::ViewLayer> layers;
    layers.push_back({sceneProject.scene, cameraEntity, viewRect, false, true, false, (float)canvasWidth, (float)canvasHeight});

    for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
        SceneProject* childScene = project.getScene(childSceneRef.id);
        if (!childSceneRef.startActive || !childScene || !childScene->scene) continue;
        const Entity childCamera = project.getGameCamera(childSceneRef.id);
        if (childCamera == NULL_ENTITY) continue;

        // an inline child was updated and drawn this frame as a layer of the Scene view
        if (!childScene->expandedInline) {
            childScene->scene->getSystem<MeshSystem>()->update(0);
            childScene->scene->getSystem<UISystem>()->update(0);
            childScene->scene->getSystem<RenderSystem>()->update(0);
        }
        layers.push_back({childScene->scene, childCamera, viewRect, false, false, false, (float)canvasWidth, (float)canvasHeight});
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
        childScene->getSystem<RenderSystem>()->update(0);
    }
    sceneRender->updateRenderSystem();

    Engine::systemDraw();

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

    if (!benchmarkCameraReady) {
        // the scene tab decides the selected scene, so bring it to front like Play does
        if (sceneWindow)
            sceneWindow->requestPlayFocus(benchmarkSceneId);
        Entity camera = selected->scene->getCamera();
        if (camera == NULL_ENTITY)
            camera = selected->mainCamera;
        if (selected->sceneRender->setPreviewCamera(camera))
            benchmarkCameraReady = true;
        if (benchmarkMeshLod >= 0)
            selected->scene->setMeshLodEnabled(benchmarkMeshLod != 0);
        if (benchmarkDepthPrepass >= 0)
            selected->scene->setDepthPrepassEnabled(benchmarkDepthPrepass != 0);
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
        if (!renderedSceneThisFrame || sceneWindow->getWidth(selected->id) <= 0)
            return;
        benchmarkPhase = 1;
        benchmarkPhaseFrames = 0;
        return;
    }

    if (!renderedSceneThisFrame)
        return;

    if (benchmarkPhase == 1) {
        if (++benchmarkPhaseFrames >= benchmarkWarmupFrames) {
            benchmarkPhase = 2;
            benchmarkPhaseFrames = 0;
            benchmarkFps.clear();
            benchmarkDraws.clear();
            benchmarkInstances.clear();
            benchmarkTris.clear();
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
        if (static_cast<int>(benchmarkFps.size()) >= benchmarkMeasureFrames)
            finishBenchmark();
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

    SceneProject* selected = project.getScene(benchmarkSceneId);
    const int width = selected && sceneWindow ? sceneWindow->getWidth(selected->id) : 0;
    const int height = selected && sceneWindow ? sceneWindow->getHeight(selected->id) : 0;

    std::ostringstream json;
    json << std::fixed;
    json.precision(2);
    json << "{\n"
         << "  \"name\": \"" << jsonEscape(benchmarkName.empty() ? (selected ? selected->name : "scene") : benchmarkName) << "\",\n"
         << "  \"scene\": \"" << jsonEscape(selected ? selected->name : benchmarkScene) << "\",\n"
         << "  \"meshLod\": " << ((selected && selected->scene && selected->scene->isMeshLodEnabled()) ? "true" : "false") << ",\n"
         << "  \"depthPrepass\": " << ((selected && selected->scene && selected->scene->isDepthPrepassEnabled()) ? "true" : "false") << ",\n"
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
         << "  \"avgTriangles\": " << avgTris << "\n"
         << "}\n";

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
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        mainThreadTasks.push(std::move(task));
    }
    // Wake an idle backend loop so cross-thread work runs promptly.
    if (wakeCallback) wakeCallback();
}

bool editor::App::hasPendingMainThreadTasks() {
    std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
    return !mainThreadTasks.empty();
}

// Counts as activity, so the loop also keeps drawing for the idle delay past the
// last request: enough for ImGui to auto-resize a window whose content just grew.
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

bool editor::App::consumeFrameRequest() {
    const bool requested = frameRequested;
    frameRequested = false;
    return requested;
}

void editor::App::setWakeCallback(std::function<void()> cb) {
    wakeCallback = cb;
    // The AI service keeps its own copy, so nothing it holds points back here.
    if (aiChatWindow) aiChatWindow->setWakeCallback(std::move(cb));
}

void editor::App::shutdownBackgroundWork() {
    loadingPump = nullptr;
    pendingProjectChange = nullptr;
    // Before glfwTerminate/SDL_Quit, or a late reply posts to a dead window system.
    if (aiChatWindow) aiChatWindow->shutdown();
    wakeCallback = nullptr;
}

void editor::App::engineViewDestroyed(){
    imageViewerWindow->closeAll();
    // Closing during project loading can leave uploads committed before any scene frame
    // consumes them. Drain that batch before shutdown waits to flush the queue.
    SystemRender::executeQueue();
    Engine::systemViewDestroyed();
}

void editor::App::engineShutdown(){
    Engine::systemShutdown();
}

void editor::App::addNewSceneToDock(uint32_t sceneId){
    if (isInitialized && !projectLoading){
        const std::string windowName = "###Scene" + std::to_string(sceneId);
        const SceneProject* sceneProject = project.getScene(sceneId);
        const bool isUnsavedScene = sceneProject && sceneProject->filepath.empty();

        dockTabWindow(windowName, isUnsavedScene);
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

    if (lastActivatedScene == sceneId) {
        resetLastActivatedScene();
    }
}

void editor::App::prepareForProjectSwitch() {
    imageViewerWindow->closeAll();
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
    dockspaceNeedsRebuild = true;
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

void editor::App::requestDockspaceRebuild() {
    dockspaceNeedsRebuild = true;
}

void editor::App::updateWindowTitle(const std::string& projectName) {
    Backend::updateWindowTitle(projectName);
}

void editor::App::stopTransientPreviews() {
    if (terrainEditWindow) {
        terrainEditWindow->endStroke();
        terrainEditWindow->updateFoliagePreview();
    }
    if (inspectorWindow) {
        inspectorWindow->stopTransientPreviews();
    }
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

void editor::App::registerAlert(std::string title, std::string message) {
    registerAlert(title, message, "");
}

void editor::App::registerAlert(std::string title, std::string message, std::string note) {
    alert.needShow = true;
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
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::Confirm;
    alert.onYes = onYes;
    alert.onNo = onNo;
}

void editor::App::registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::ThreeButton;
    alert.onYes = onYes;
    alert.onNo = onNo;
    alert.onCancel = onCancel;
}

void editor::App::registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback) {
    // Add scene to the save dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Scene, sceneId, callback};
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

void editor::App::registerProjectSaveDialog(std::function<void()> callback) {
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
    return App::getUserCacheBaseDir() / "jasperengine" / "shaders" / "v29";
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

editor::TerrainEditWindow* editor::App::getTerrainEditWindow() const{
    return terrainEditWindow;
}

editor::HierarchyWindow* editor::App::getHierarchyWindow() const{
    return hierarchyWindow;
}

editor::GameWindow* editor::App::getGameWindow() const{
    return gameWindow;
}

editor::MainToolbar* editor::App::getMainToolbar() const{
    return mainToolbar;
}

bool editor::App::isSceneWindowFocused() const{
    return sceneWindow && sceneWindow->isFocused();
}

void editor::App::resetLayout(){
    executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::ResetLayout), {}});
}

void editor::App::processNextSaveDialog() {
    // Check if there's anything to process and no dialogs are currently open
    if (saveDialogInProgress || saveDialogQueue.empty() || sceneSaveDialog.isOpen() || projectSaveDialog.isOpen()) {
        return;
    }

    // Get the next item from the queue
    SaveDialogQueueItem item = saveDialogQueue.front();
    // Store the callback to use it later
    std::function<void()> completionCallback = item.callback;

    if (item.type == SaveDialogType::Scene) {
        // Process scene save dialog
        uint32_t sceneId = item.sceneId;
        SceneProject* sceneProject = project.getScene(sceneId);
        if (!sceneProject) {
            // Invalid scene, remove from queue and try next item
            popSaveDialogQueueItem();
            processNextSaveDialog();
            return;
        }

        // Set default filename
        std::string defaultName = sceneProject->name + ".scene";
        fs::path initialDirectory = project.getProjectPath();
        if (projectWindow) {
            fs::path resourcesPath = projectWindow->getCurrentPath();
            if (!resourcesPath.empty()) {
                initialDirectory = resourcesPath;
            }
        }

        saveDialogInProgress = true;

        // Open dialog for the current scene
        sceneSaveDialog.open(
            project.getProjectPath(),
            initialDirectory,
            defaultName,
            // Save callback
            [this, sceneId, completionCallback](const fs::path& fullPath) {
                bool saveStarted = false;
                SceneProject* sceneProject = project.getScene(sceneId);
                if (sceneProject) {
                    // Create directory if it doesn't exist
                    std::filesystem::create_directories(fullPath.parent_path());

                    // Save the scene
                    std::error_code ec;
                    fs::path relPath = fs::relative(fullPath, project.getProjectPath(), ec);
                    if (ec || relPath.empty()) {
                        Out::error("Scene filepath must be relative to project path: %s", fullPath.string().c_str());
                        Backend::getApp().registerAlert("Error", "Scene file must be inside the project folder.");
                    } else {
                        sceneProject->filepath = relPath;
                        saveStarted = true;
                        project.saveSceneToPathAsync(sceneId, fullPath, [this, completionCallback](bool success) {
                            saveDialogInProgress = false;
                            popSaveDialogQueueItem();

                            if (success && completionCallback) {
                                completionCallback();
                            }

                            processNextSaveDialog();
                        });
                    }
                }

                if (!saveStarted) {
                    saveDialogInProgress = false;
                    popSaveDialogQueueItem();
                    processNextSaveDialog();
                }
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
    else if (item.type == SaveDialogType::Project) {
        // Process project save dialog
        std::string defaultName = project.getName();
        if (defaultName.empty()) {
            defaultName = "MyProject";
        }

        saveDialogInProgress = true;

        projectSaveDialog.open(
            defaultName,
            // Save callback
            [this, completionCallback](const std::string& projectName, const fs::path& projectPath) {
                // Set the project name if provided
                if (!projectName.empty()) {
                    project.setName(projectName);
                }

                // Save the project to the selected path
                const bool saved = project.saveProjectToPath(projectPath);
                if (!saved) registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");

                // Remove this item from the queue
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Execute the completion callback if provided
                if (saved && completionCallback) {
                    completionCallback();
                }

                // Process the next item if available
                processNextSaveDialog();
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
}

void editor::App::processMainThreadTasks() {
    if (projectLoading) return;

    std::queue<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        std::swap(tasks, mainThreadTasks);
    }

    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop();
        if (task) task();
    }
}

bool editor::App::isMainThread() const {
    return std::this_thread::get_id() == mainThreadId;
}

void editor::App::initializeSettings() {
    AppSettings::initialize();

    // Next to settings.yaml, where a user can find it to report a crash.
    const std::filesystem::path logFile = AppSettings::getConfigDirectory() / "editor.log";
    Out::setLogFile(logFile);
    CrashHandler::install(logFile);

    applyPanelVisibilitySettings();
}

void editor::App::applyPanelVisibilitySettings() {
    const PanelVisibilitySettings visibility = AppSettings::getPanelVisibility();
    hierarchyWindow->setOpen(visibility.hierarchy);
    inspectorWindow->setOpen(visibility.inspector);
    projectWindow->setOpen(visibility.project);
    consoleWindow->setOpen(visibility.console);
    animationWindow->setOpen(visibility.animation);
    terrainEditWindow->setOpen(visibility.terrain);
    aiChatWindow->setOpen(visibility.aiChat);
    gameWindow->setOpen(visibility.game);
}

void editor::App::persistPanelVisibilitySettings() {
    PanelVisibilitySettings visibility;
    visibility.hierarchy = hierarchyWindow->isOpen();
    visibility.inspector = inspectorWindow->isOpen();
    visibility.project = projectWindow->isOpen();
    visibility.console = consoleWindow->isOpen();
    visibility.animation = animationWindow->isOpen();
    visibility.terrain = terrainEditWindow->isOpen();
    visibility.aiChat = aiChatWindow->isOpen();
    visibility.game = gameWindow->isOpen();

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
        AppSettings::setLayoutUiScale(sanitizeUiScale(layoutUiScale));
    }
    AppSettings::saveSettings();
}

void editor::App::exit() {
    if (projectLoading) {
        Backend::closeWindow();
        return;
    }

    // Check if any modal popup is currently open (including ComponentAddDialog, ScriptCreateDialog, etc.)
    ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal();
    if (modal != nullptr) {
        // A modal is open - close it first
        ImGui::CloseCurrentPopup();
        return;  // Don't proceed with exit yet, user needs to click close again
    }

    // Also check if the scene save dialog is specifically open
    if (sceneSaveDialog.isOpen()) {
        sceneSaveDialog.close();
        return;
    }

    if (project.isAnySceneSaving()) {
        registerAlert("Saving Scene", "Wait for the current scene save to finish before exiting.");
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
