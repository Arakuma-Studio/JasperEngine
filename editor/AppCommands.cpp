// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The command catalog's actions (S25 R2-R4): what each command of shortcuts/DefaultCommands.cpp
// runs, when it is enabled (the main menu shows the same state) and the keyboard-only
// conditions it had before the catalog. Then the one dispatcher (R3) and the editor scripts'
// menu items as commands (S23 R42).

#include "App.h"

#include "AppMenuCommand.h"
#include "Backend.h"
#include "Out.h"
#include "animation/AnimationClipAppHooks.h"
#include "lattice/LatticeSceneTools.h"
#include "pose/PoseViewportLogic.h"
#include "window/AnimationWindowClip.h"
#include "window/PoseAnimationWindow.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavPathTester.h"
#include "sceneview/SceneViewController.h"
#include "shortcuts/DefaultCommands.h"
#include "util/FluidEditorTools.h"
#include "shortcuts/ShortcutOverrides.h"
#include "util/EditorScripts.h"
#include "util/EntityClipboard.h"
#include "util/ScriptReloadService.h"
#include "window/InputActionsWindow.h"
#include "window/JobsMemoryWindow.h"
#include "window/vcs/VcsUi.h"

#include "imgui_internal.h"

using namespace jasperengine;

namespace {

    editor::PlatformMenuCommand menuCommandOf(editor::AppMenuCommand command, std::string payload = {}) {
        return {static_cast<uint32_t>(command), std::move(payload)};
    }

}

void editor::App::registerCommands(){
    CommandCatalog& catalog = CommandCatalog::editor();
    for (CommandDef& def : defaultCommands()) {
        if (def.menuCommand) {
            const uint32_t id = *def.menuCommand;
            const std::string payload = def.menuPayload;
            def.run = [this, id, payload](const CommandArgs& args) {
                executeMenuCommand({id, args.payload.empty() ? payload : args.payload});
            };
        }
        catalog.add(std::move(def));
    }

    auto bind = [&catalog](const char* id, std::function<void(const CommandArgs&)> run, std::function<bool()> enabled = nullptr,
                           std::function<bool()> keyGate = nullptr) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        if (run) def.run = std::move(run);
        if (enabled) def.enabled = std::move(enabled);
        if (keyGate) def.keyGate = std::move(keyGate);
        catalog.add(std::move(def));
    };
    auto enable = [&bind](const char* id, std::function<bool()> enabled, std::function<bool()> keyGate = nullptr) {
        bind(id, nullptr, std::move(enabled), std::move(keyGate));
    };

    bind("assets.clearShaderWarmup", [this](const CommandArgs&) { project.clearShaderWarmup(); });

    auto busy = [this]() { return project.isAnyScenePlaying(); };
    auto notBusy = [busy]() { return !busy(); };
    // Duplicate, Delete and Rename leave the Project window and the Jobs & Memory panel to
    // their own keys (as before the catalog)
    auto notAssetPanel = [this]() { return !projectWindow->isFocused() && !jobsMemoryWindow->isFocused(); };
    auto noDrag = []() { return !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsAnyItemActive(); };
    // the scene's editing keys stay off while the Animation window editing a clip or the Pose
    // Animation window (or a Scene view posing for it) has the keys: F2 would move the focus to
    // the Inspector's name, Delete / Ctrl+D act on the poses and the clip keys there
    auto sceneKeys = [this]() { return !clipWindowTakesKeys() && lastFocusedWindow != LastFocusedWindow::PoseAnimation; };
    auto sceneKeysAnd = [sceneKeys](std::function<bool()> gate) {
        return [sceneKeys, gate]() { return sceneKeys() && (!gate || gate()); };
    };

    // ---- Assets > Version Control (S29 R29): the Project window's selection; a disabled item
    // says why (VcsUi::canRun, the menu's tooltip)
    auto vcsCommand = [this, &catalog](const char* id, vcs::VcsAction action) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.enabled = [this, action]() { return vcsUi->canRun(action, projectWindow->getSelectedFiles(), nullptr); };
        def.disabledReason = [this, action]() {
            std::string why;
            vcsUi->canRun(action, projectWindow->getSelectedFiles(), &why);
            return why;
        };
        catalog.add(std::move(def));
    };
    vcsCommand("assets.vcs.commit", vcs::VcsAction::Commit);
    vcsCommand("assets.vcs.revert", vcs::VcsAction::Revert);
    vcsCommand("assets.vcs.lock", vcs::VcsAction::Lock);
    vcsCommand("assets.vcs.unlock", vcs::VcsAction::Unlock);
    vcsCommand("assets.vcs.history", vcs::VcsAction::ShowHistory);
    vcsCommand("assets.vcs.diff", vcs::VcsAction::DiffAgainstBaseline);
    vcsCommand("assets.vcs.refresh", vcs::VcsAction::Refresh);

    // ---- File
    enable("file.newScene3D", notBusy);
    enable("file.newScene2D", notBusy);
    enable("file.newSceneUI", notBusy);
    enable("file.save", [this]() {
        if (saveDialogInProgress) return false;
        if (project.isTempProject()) return !project.isAnyScenePlaying();
        if (lastFocusedWindow == LastFocusedWindow::Code) return codeEditor->hasLastFocusedUnsavedChanges();
        return !project.isAnyScenePlaying() && project.hasSelectedSceneUnsavedChanges();
    });
    auto canSaveAll = [this]() { return !project.isAnyScenePlaying() && !saveDialogInProgress; };
    enable("file.saveAll", canSaveAll);
    enable("file.saveProjectAs", canSaveAll);
    enable("file.removeScene", [this]() {
        return project.getSelectedScene() && !project.isAnyScenePlaying() && project.getScenes().size() > 1;
    });
    enable("file.newProject", notBusy);
    enable("file.openProject", notBusy);

    // ---- Edit: one timeline whatever window had focus (S24 R21); a text field and the Code
    // Editor keep their own undo, so the keys leave them alone
    auto undoEnabled = [this](bool undo) {
        switch (undoTarget()) {
            case UndoTimeline::Target::TextField:
                return false;
            case UndoTimeline::Target::CodeEditor:
                return undo ? codeEditor->canUndoLastFocused() : codeEditor->canRedoLastFocused();
            case UndoTimeline::Target::Timeline:
                break;
        }
        const uint32_t sceneId = project.getSelectedScene() ? project.getSelectedSceneId() : NULL_PROJECT_SCENE;
        return undo ? UndoTimeline::canUndo(sceneId) : UndoTimeline::canRedo(sceneId);
    };
    auto timelineKeys = [this]() { return undoTarget() == UndoTimeline::Target::Timeline; };
    enable("edit.undo", [undoEnabled]() { return undoEnabled(true); }, timelineKeys);
    enable("edit.redo", [undoEnabled]() { return undoEnabled(false); }, timelineKeys);
    enable("edit.selectAll", [this]() { return canSelectAll(); }, sceneKeys);
    enable("edit.cut", [this]() { return canCutSelection(); }, sceneKeys);
    enable("edit.copy", [this]() { return canCopySelection(); }, sceneKeys);
    enable("edit.paste", [this]() { return canPasteSelection(false); }, sceneKeys);
    enable("edit.pasteAsChild", [this]() { return canPasteSelection(true); }, sceneKeys);
    enable("edit.duplicate", [this]() { return canEditSelection(true); }, sceneKeysAnd(notAssetPanel));
    // the Input Actions window, focused, deletes its selected map, action or binding
    enable("edit.delete", [this]() { return canEditSelection(false) || inputActionsWindow->canDelete(); }, sceneKeysAnd(notAssetPanel));
    // F2 renames inline in the Hierarchy when it has focus, else in the Inspector
    bind("edit.rename", [this](const CommandArgs& args) {
        if (args.source == CommandSource::Keyboard) {
            if (!(hierarchyWindow->isFocused() && hierarchyWindow->renameSelected())) inspectorWindow->focusNameInput();
            return;
        }
        executeMenuCommand(menuCommandOf(AppMenuCommand::RenameSelection));
    }, [this]() {
        const uint32_t sceneId = project.getSelectedSceneId();
        return !ImGui::GetIO().WantTextInput && !project.isAnyScenePlaying() && lastFocusedWindow == LastFocusedWindow::AnySceneWindow &&
               project.getSelectedScene() && project.getSelectedEntities(sceneId).size() == 1;
    }, sceneKeysAnd(notAssetPanel));
    enable("edit.frameSelected", [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && !ImGui::GetIO().WantTextInput && !project.isAnyScenePlaying() &&
               lastFocusedWindow == LastFocusedWindow::AnySceneWindow && sceneWindow->hasFrameableSelection(project.getSelectedSceneId());
    }, sceneKeysAnd(noDrag));
    enable("edit.play", [this]() { return mainToolbar->canTogglePlay(); });
    enable("edit.pause", [this]() { return mainToolbar->canPause(); });
    enable("edit.step", [this]() { return mainToolbar->canStep(); });
    enable("edit.stop", [this]() { return mainToolbar->isInPlayMode(); });
    // the palette is not a modal: it opens over anything but one
    enable("edit.searchAll", [this]() { return !projectLoading && ImGui::GetTopMostAndVisiblePopupModal() == nullptr; });

    // ---- Play Mode keys
    bind("playmode.playOrResume", [this](const CommandArgs&) { mainToolbar->playOrResume(); },
         [this]() { return mainToolbar->canPlayOrResume(); });
    bind("playmode.pauseOnly", [this](const CommandArgs&) { mainToolbar->pauseOnly(); },
         [this]() { return mainToolbar->canPause() && !mainToolbar->isPaused(); });
    bind("playmode.stop", [this](const CommandArgs&) { mainToolbar->stopSession(); },
         [this]() { return mainToolbar->canStopSession(); });

    // ---- Assets
    enable("assets.open", [this]() { return projectWindow->hasSingleAssetSelection(); });
    enable("assets.rename", [this]() { return projectWindow->hasSingleAssetSelection(); });
    enable("assets.delete", [this]() { return projectWindow->hasAssetSelection(); });
    enable("assets.createAnimationClip", []() { return static_cast<bool>(animationClipAppHooks().createClipAsset); });
    enable("assets.migrateReferences", notBusy);
    enable("assets.scenesInBuild", notBusy);
    enable("assets.emptyTrash", notBusy);
    enable("scripts.reload", busy);
    enable("scripts.rebuildCpp", []() { return !ScriptReloadService::get().isCppBuilding(); });
    enable("scripts.cancelBuild", []() { return ScriptReloadService::get().isCppBuilding(); });

    // ---- GameObject
    const auto& entries = HierarchyWindow::getCreateMenuEntries();
    for (size_t i = 0; i < entries.size(); i++) {
        enable(createEntryCommandId(i).c_str(), [this, i]() {
            const SceneProject* scene = project.getSelectedScene();
            return scene && scene->scene && HierarchyWindow::isCreateEntryAvailable(HierarchyWindow::getCreateMenuEntries()[i], scene->sceneType);
        });
    }
    enable("gameobject.createEmptyChild", [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && scene->scene && hierarchyWindow->getSelectedParentForCreate() != NULL_ENTITY;
    });
    enable("component.add", [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && !project.isAnyScenePlaying() && !project.getSelectedEntities(scene->id).empty();
    });

    // ---- Window
    enable("window.scene", [this]() { return project.getSelectedScene() != nullptr; });

    // ---- Tools: not while flying the 3D camera (Right Mouse + WASDQE) or dragging a field,
    // nor while the Pose Animation window (or a Scene view posing its character) or the
    // Animation window editing a clip has the keys: their letters are theirs
    auto toolKeys = [this, noDrag]() { return noDrag() && !animationOrPoseOwnsLetterKeys(); };
    auto tool = [this, &bind, toolKeys](const char* id, MainToolbar::Tool which) {
        bind(id, [this, which](const CommandArgs&) { mainToolbar->setTool(which); },
             [this, which]() { return mainToolbar->canUseTool(which); }, toolKeys);
    };
    tool("tools.hand", MainToolbar::Tool::Hand);
    tool("tools.move", MainToolbar::Tool::Move);
    tool("tools.rotate", MainToolbar::Tool::Rotate);
    tool("tools.scale", MainToolbar::Tool::Scale);
    tool("tools.rect", MainToolbar::Tool::Rect);
    bind("tools.toggleGlobalLocal", [this](const CommandArgs&) { mainToolbar->toggleGlobalTransform(); },
         [this]() { return project.getSelectedScene() != nullptr; }, toolKeys);
    // Physics (G): a modifier of Move/Rotate/Scale; turning it off always works
    bind("tools.physics", [this](const CommandArgs&) { mainToolbar->togglePhysicsMode(); },
         [this]() { return mainToolbar->isPhysicsMode() || mainToolbar->canUsePhysics(); }, toolKeys);

    // ---- Scene View: the selected scene's view, a 3D one for the camera keys
    auto scene3D = [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && scene->sceneType == SceneType::SCENE_3D && scene->sceneRender;
    };
    // the selected scene's view, any type (S26)
    auto sceneShown = [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && scene->sceneRender;
    };
    bind("sceneView.frameAll", [this](const CommandArgs&) { sceneWindow->frameAll(project.getSelectedSceneId()); }, sceneShown, noDrag);
    auto onView = [this, &bind, sceneShown, noDrag](const char* id, std::function<void(SceneViewController&, SceneProject&)> action,
                                                     std::function<bool(SceneViewController&, SceneProject&)> when = nullptr) {
        bind(id, [this, action](const CommandArgs&) {
            if (SceneProject* scene = project.getSelectedScene()) action(sceneWindow->getViewController(), *scene);
        }, [this, sceneShown, when]() {
            SceneProject* scene = project.getSelectedScene();
            return sceneShown() && (!when || when(sceneWindow->getViewController(), *scene));
        }, noDrag);
    };
    onView("sceneView.hideSelection", [](SceneViewController& view, SceneProject& scene) { view.hideSelection(scene); });
    onView("sceneView.showAll", [](SceneViewController& view, SceneProject& scene) { view.showAll(scene); });
    onView("sceneView.togglePickLock", [](SceneViewController& view, SceneProject& scene) { view.togglePickLockSelection(scene); });
    onView("sceneView.measure", [](SceneViewController& view, SceneProject& scene) { view.setMeasuring(scene, !view.isMeasuring(scene.id)); });
    // the Path Tester (S54 R25): in the selected scene's view, edit or play
    bind("navigation.pathTester", [this](const CommandArgs&) {
        if (SceneProject* scene = project.getSelectedScene()) NavPathTester::get().toggle(*scene);
    }, sceneShown, noDrag);
    onView("sceneView.deleteMeasure", [](SceneViewController& view, SceneProject& scene) { view.deleteMeasure(scene, view.selectedMeasure(scene.id)); },
           [](SceneViewController& view, SceneProject& scene) { return view.hasSelectedMeasure(scene.id); });
    for (int slot = 1; slot <= 9; slot++) {
        const std::string number = std::to_string(slot);
        onView(("sceneView.recallBookmark" + number).c_str(), [slot](SceneViewController& view, SceneProject& scene) {
            std::string error;
            view.recallBookmark(scene, slot, error);
        });
        onView(("sceneView.storeBookmark" + number).c_str(), [slot](SceneViewController& view, SceneProject& scene) {
            std::string error;
            view.storeBookmark(scene, slot, error);
        });
    }
    auto view = [this, &bind, scene3D, noDrag](const char* id, Vector3 direction) {
        bind(id, [this, direction](const CommandArgs&) { sceneWindow->viewFrom(project.getSelectedSceneId(), direction); }, scene3D, noDrag);
    };
    view("sceneView.viewFront", Vector3(0, 0, 1));
    view("sceneView.viewBack", Vector3(0, 0, -1));
    view("sceneView.viewRight", Vector3(-1, 0, 0));
    view("sceneView.viewLeft", Vector3(1, 0, 0));
    view("sceneView.viewTop", Vector3(0, -1, 0));
    view("sceneView.viewBottom", Vector3(0, 1, 0));

    // ---- Terrain brush, while the Terrain window edits the selected scene's terrain
    auto terrainEditing = [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && scene->scene && terrainEditWindow->isEditingScene(scene->scene);
    };
    // Paint Dye (a fluid's brush) takes [ and ] for its radius while it is armed
    auto dyePainting = [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && scene->sceneRender && scene->sceneRender->isFluidPainting();
    };
    auto brush = [this, &bind, terrainEditing, dyePainting, noDrag](const char* id, bool size, float factor) {
        bind(id, [this, size, factor, dyePainting](const CommandArgs&) {
            const SceneProject* selected = project.getSelectedScene();
            // the nav-grid brush (S19): a cell more or less across
            if (size && selected && NavigationEditor::get().isBrushPainting(selected->scene)) {
                NavigationEditor::Brush& navBrush = NavigationEditor::get().getBrush();
                navBrush.size = std::clamp(navBrush.size + (factor > 1.0f ? 1 : -1), 1, 64);
            } else if (size && dyePainting()) FluidEditorTools::adjustRadius(factor);
            else if (size) terrainEditWindow->adjustBrushSize(factor);
            else terrainEditWindow->adjustBrushStrength(factor);
        }, [terrainEditing, dyePainting, size]() { return terrainEditing() || (size && dyePainting()); }, noDrag);
    };
    brush("terrain.brushSmaller", true, 1.0f / 1.15f);
    brush("terrain.brushLarger", true, 1.15f);
    brush("terrain.brushWeaker", false, 1.0f / 1.15f);
    brush("terrain.brushStronger", false, 1.15f);

    // ---- Lattice points, while the selected scene edits a lattice's points
    auto points = [this, &bind, noDrag](const char* id, LatticeSceneTools::PointAction action) {
        bind(id, [this, action](const CommandArgs&) { LatticeSceneTools::runPointAction(&project, project.getSelectedSceneId(), action); },
             [this]() { return LatticeSceneTools::isEditingPoints(&project, project.getSelectedSceneId()); }, noDrag);
    };
    points("lattice.selectNone", LatticeSceneTools::PointAction::SelectNone);
    points("lattice.invert", LatticeSceneTools::PointAction::Invert);
    points("lattice.expand", LatticeSceneTools::PointAction::Expand);
    points("lattice.shrink", LatticeSceneTools::PointAction::Shrink);
    points("lattice.toggleFalloff", LatticeSceneTools::PointAction::ToggleFalloff);

    // ---- Project window: its file operations, not while typing in its search field
    auto projectKeys = []() { return !ImGui::GetIO().WantTextInput; };
    bind("project.copy", [this](const CommandArgs&) { projectWindow->copySelection(false); },
         [this]() { return projectWindow->hasAssetSelection(); }, projectKeys);
    bind("project.cut", [this](const CommandArgs&) { projectWindow->copySelection(true); },
         [this]() { return projectWindow->hasAssetSelection(); }, projectKeys);
    bind("project.paste", [this](const CommandArgs&) { projectWindow->pasteClipboard(); },
         [this]() { return projectWindow->canPasteClipboard(); }, projectKeys);
    bind("project.selectAll", [this](const CommandArgs&) { projectWindow->selectAllInListing(); }, nullptr, projectKeys);
    bind("project.delete", [this](const CommandArgs&) { projectWindow->requestDeleteSelection(); },
         [this]() { return projectWindow->hasAssetSelection(); }, projectKeys);
    bind("project.rename", [this](const CommandArgs&) { projectWindow->renameSelectedOrHovered(); }, nullptr, projectKeys);

    // ---- Console
    bind("console.copy", [this](const CommandArgs&) { consoleWindow->copySelectedMessage(); },
         [this]() { return consoleWindow->hasSelectedMessage(); });

    // ---- Animation window (clip mode) and Pose Animation window: their own commands
    AnimationWindowClip::bindCommands(catalog, [this]() { return animationWindow->getClipMode(); });
    poseAnimationWindow->bindCommands(catalog, [this]() { return poseKeysActive(); }, [this]() { return poseKeysFromSceneView(); });

    ShortcutOverrides::load(ShortcutOverrides::defaultFile(), catalog);
}

bool editor::App::clipWindowTakesKeys() const{
    return animationClipAppHooks().takesKeys && animationClipAppHooks().takesKeys();
}

bool editor::App::poseKeysFromSceneView() const{
    // a posing Scene view gets the window's keys, but not while its camera flies or orbits
    // (right/middle mouse: W A S D Q E move it)
    return !poseAnimationWindow->isFocused() && lastFocusedWindow == LastFocusedWindow::PoseAnimation &&
           poseview::sceneViewTakesPoseKeys(ImGui::IsMouseDown(ImGuiMouseButton_Right), ImGui::IsMouseDown(ImGuiMouseButton_Middle),
                                            sceneWindow->isNavigatingCamera());
}

bool editor::App::poseKeysActive() const{
    return poseAnimationWindow->isFocused() || poseKeysFromSceneView();
}

bool editor::App::animationOrPoseOwnsLetterKeys() const{
    // a Scene view posing the window's character keeps its letter keys for the pose (E: Global Edit)
    const bool scenePosing = sceneWindow->isFocused() && poseAnimationWindow->ownsUndo(true) && !poseAnimationWindow->isFocused();
    return poseAnimationWindow->isFocused() || scenePosing || clipWindowTakesKeys();
}

void editor::App::syncScriptCommands(){
    const uint64_t revision = EditorScripts::get().revision();
    if (revision == scriptCommandsRevision) return;
    scriptCommandsRevision = revision;
    CommandCatalog& catalog = CommandCatalog::editor();
    catalog.removeDynamic("script.");
    for (const EditorMenuItem& item : EditorScripts::get().items()) {
        if (item.kind != "menu") continue;
        CommandDef def;
        def.id = "script." + item.path;
        const size_t slash = item.path.find('/');
        def.group = slash == std::string::npos ? std::string("Scripts") : item.path.substr(0, slash);
        def.name = item.name();
        def.context = ShortcutContext::Editor;
        def.primary = Chord::parse(item.shortcut).value_or(Chord());
        def.description = item.tooltip.empty() ? "Editor script: " + item.file : item.tooltip;
        def.dynamic = true;
        def.menuCommand = static_cast<uint32_t>(AppMenuCommand::ScriptMenu);
        def.menuPayload = item.id;
        const std::string itemId = item.id;
        def.run = [this, itemId](const CommandArgs&) { executeMenuCommand(menuCommandOf(AppMenuCommand::ScriptMenu, itemId)); };
        def.enabled = [itemId]() {
            const EditorMenuItem* script = EditorScripts::get().find(itemId);
            return script && EditorScripts::get().isEnabled(*script);
        };
        catalog.add(std::move(def));
        catalog.applyStoredOverride("script." + item.path);
    }
}

editor::ShortcutState editor::App::shortcutState() const{
    ShortcutState state;
    const ImGuiIO& io = ImGui::GetIO();
    state.wantTextInput = io.WantTextInput;
    state.codeEditorFocused = codeEditor->isFocused();
    state.gameHasKeyboard = project.isAnyScenePlaying() && gameWindow->hasKeyboardFocus();
    // the Input Actions window's Listen takes the next key for a game binding
    state.popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                      inputActionsWindow->isListening();
    state.nativeMenuOwnsKeys = Backend::nativeMenuHandlesKeyEquivalents();
    if (hierarchyWindow->isFocused()) state.panels.push_back(ShortcutContext::Hierarchy);
    if (sceneWindow->isFocused() || sceneWindow->isHovered()) state.panels.push_back(ShortcutContext::SceneView);
    if (projectWindow->isFocused()) state.panels.push_back(ShortcutContext::Project);
    if (consoleWindow->isListFocused()) state.panels.push_back(ShortcutContext::Console);
    if (clipWindowTakesKeys()) state.panels.push_back(ShortcutContext::Animation);
    if (poseKeysActive()) state.panels.push_back(ShortcutContext::PoseAnimation);
    return state;
}

void editor::App::processShortcuts(){
    syncScriptCommands();
    shortcutDispatcher.process(ImGuiShortcutKeys(), shortcutState());
    // Esc of a Scene view posing the Pose Animation window's character (a local key; the
    // window runs it itself while focused)
    if (poseKeysFromSceneView()) poseAnimationWindow->handleLocalKeys();
}
