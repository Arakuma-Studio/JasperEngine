// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The command catalog's actions (S25 R2-R4): what each command of shortcuts/DefaultCommands.cpp
// runs, when it is enabled (the main menu shows the same state) and the keyboard-only
// conditions it had before the catalog. Then the one dispatcher (R3) and the editor scripts'
// menu items as commands (S23 R42).

#include "App.h"
#include "window/AudioCapturesWindow.h"
#include "window/SoundBrowserWindow.h"
#include "window/HierarchyWindow.h"
#include "window/InspectorWindow.h"
#include "window/MainToolbar.h"
#include "window/ProjectWindow.h"
#include "render/SceneRender.h"
#include "window/AnimationWindow.h"
#include "window/AnimatorWindow.h"
#include "window/GameWindow.h"
#include "window/SpriteNormalsWindow.h"
#include "window/TerrainEditWindow.h"

#include "AppMenuCommand.h"
#include "Backend.h"
#include "Out.h"
#include "Engine.h"
#include "util/Util.h"
#include "window/PlayInput.h"
#include "animation/AnimationClipAppHooks.h"
#include "lattice/LatticeSceneTools.h"
#include "pose/PoseViewportLogic.h"
#include "window/AnimationWindowClip.h"
#include "window/PoseAnimationWindow.h"
#include "window/SpritePreviewWindow.h"
#include "navigation/NavigationEditor.h"
#include "sceneview/SceneViewController.h"
#include "search/SearchPalette.h"
#include "shortcuts/DefaultCommands.h"
#include "shortcuts/PanelClaims.h"
#include "util/FluidEditorTools.h"
#include "shortcuts/ShortcutOverrides.h"
#include "util/EditorScripts.h"
#include "util/EntityClipboard.h"
#include "util/ScriptReloadService.h"
#include "widget/ColorPickerWindow.h"
#include "widget/CurveField.h"
#include "widget/GradientField.h"
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
    auto noDrag = []() { return !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsAnyItemActive(); };

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
        // an untitled scene saves even unchanged: the dialog asks for its file (Unity)
        return !project.isAnyScenePlaying() && project.sceneNeedsSave(project.getSelectedSceneId());
    });
    auto canSaveAll = [this]() { return !project.isAnyScenePlaying() && !saveDialogInProgress; };
    enable("file.saveAll", canSaveAll);
    // a shortcut pressed while they cannot run says why (the status bar), as the menu's tooltip
    auto saveDisabledReason = [this]() -> std::string {
        if (project.isAnyScenePlaying()) return "Stop Play to save (changes made in Play mode are not kept)";
        if (saveDialogInProgress) return "A Save dialog is open";
        return {};
    };
    for (const char* id : {"file.save", "file.saveAll"}) {
        if (const CommandDef* existing = catalog.find(id)) {
            CommandDef def = *existing;
            def.disabledReason = saveDisabledReason;
            catalog.add(std::move(def));
        }
    }
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
    // what these do while no panel claims them (registerPanelClaims)
    enable("edit.selectAll", [this]() { return canSelectAll(); });
    enable("edit.cut", [this]() { return canCutSelection(); });
    enable("edit.copy", [this]() { return canCopySelection(); });
    enable("edit.paste", [this]() { return canPasteSelection(false); });
    enable("edit.pasteAsChild", [this]() { return canPasteSelection(true); });
    enable("edit.duplicate", [this]() { return canEditSelection(true); });
    enable("edit.delete", [this]() { return canEditSelection(false); });
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
    });
    enable("edit.frameSelected", [this]() {
        const SceneProject* scene = project.getSelectedScene();
        return scene && !ImGui::GetIO().WantTextInput && !project.isAnyScenePlaying() &&
               lastFocusedWindow == LastFocusedWindow::AnySceneWindow && sceneWindow->hasFrameableSelection(project.getSelectedSceneId());
    }, noDrag);
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
    bind("playmode.releaseCursor", [this](const CommandArgs&) { playInput->releaseCursor(); },
         [this]() {
             const MouseMode mode = Engine::getMouseMode();
             return project.isPlaySessionActive() && !playInput->isCursorReleased() &&
                    (mode == MouseMode::CAPTURED || mode == MouseMode::CONFINED);
         });

    // ---- Assets
    enable("assets.open", [this]() { return projectWindow->hasSingleAssetSelection(); });
    enable("assets.rename", [this]() { return projectWindow->hasSingleAssetSelection(); });
    enable("assets.delete", [this]() { return projectWindow->hasAssetSelection(); });
    enable("assets.createAnimationClip", []() { return static_cast<bool>(animationClipAppHooks().createClipAsset); });
    enable("assets.createSoundEventFromClips", [this]() { return projectWindow->hasAudioSelection(); });
    bind("soundEvent.audition", [this](const CommandArgs&) { inspectorWindow->requestSoundEventAudition(); },
         [this]() { return Util::isSoundEventFile(project.getInspectedAsset().string()); });
    bind("soundEvent.matchLevels", [this](const CommandArgs&) { inspectorWindow->requestSoundEventMatchLevels(); },
         [this]() { return Util::isSoundEventFile(project.getInspectedAsset().string()); });
    bind("soundEvent.playAsMusic", [this](const CommandArgs&) { inspectorWindow->requestSoundEventPlayAsMusic(); },
         [this]() { return Util::isSoundEventFile(project.getInspectedAsset().string()) && !project.isAnyScenePlaying(); });
    bind("audioClip.detectTempo", [this](const CommandArgs&) { inspectorWindow->requestAudioClipDetectTempo(); },
         [this]() { return Util::isAudioFile(project.getInspectedAsset().string()); });
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
    // (the Sprite Normal Map window, focused, has its letters too: handleShortcuts)
    auto toolKeys = [this, noDrag]() { return noDrag() && !animationOrPoseOwnsLetterKeys() && !spriteNormalsWindow->isFocused(); };
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
    onView("sceneView.deleteMeasure", [](SceneViewController& view, SceneProject& scene) { view.deleteMeasure(scene, view.selectedMeasure(scene.id)); },
           [](SceneViewController& view, SceneProject& scene) { return view.hasSelectedMeasure(scene.id); });
    // under the pointer it must not take Delete from a panel that claims it
    bind("sceneView.deleteMeasure", nullptr, nullptr, [this, noDrag]() {
        return noDrag() && (sceneWindow->isFocused() || !PanelClaims::editor().active());
    });
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
    spritePreviewWindow->bindCommands(catalog, [this]() { return spritePreviewWindow->isFocused(); });

    // ---- The Sound Browser, while it has focus: keys typed into its search stay text
    auto browser = [&bind, this](const char* id, std::function<void()> run, bool whileTyping = false) {
        bind(id, [run](const CommandArgs&) { run(); }, [this]() { return soundBrowserWindow->isOpen(); },
             [whileTyping]() { return whileTyping || !ImGui::GetIO().WantTextInput; });
    };
    browser("soundBrowser.focusSearch", [this]() { soundBrowserWindow->focusSearch(); }, true);
    browser("soundBrowser.next", [this]() { soundBrowserWindow->selectNext(1); });
    browser("soundBrowser.previous", [this]() { soundBrowserWindow->selectNext(-1); });
    browser("soundBrowser.play", [this]() { soundBrowserWindow->playOrStop(false); });
    browser("soundBrowser.playMatched", [this]() { soundBrowserWindow->playOrStop(true); });
    browser("soundBrowser.use", [this]() { soundBrowserWindow->useSelected(); });
    browser("soundBrowser.selectAll", [this]() { soundBrowserWindow->selectAll(); });
    browser("soundBrowser.stop", [this]() { soundBrowserWindow->stopOrClear(); }, true);

    // ---- Window > Audio > Captures, while it has focus
    auto captures = [&bind, this](const char* id, std::function<void()> run) {
        bind(id, [run](const CommandArgs&) { run(); }, [this]() { return audioCapturesWindow->isOpen(); }, []() { return !ImGui::GetIO().WantTextInput; });
    };
    captures("audioCaptures.playMoment", [this]() { audioCapturesWindow->playMoment(); });
    captures("audioCaptures.stop", [this]() { audioCapturesWindow->stopMoment(); });
    captures("audioCaptures.nextMoment", [this]() { audioCapturesWindow->selectMoment(1); });
    captures("audioCaptures.previousMoment", [this]() { audioCapturesWindow->selectMoment(-1); });

    // ---- The Curve window and the Color window, while they have focus
    using CurveCommand = ui::CurveEditor::Command;
    auto curve = [&bind](const char* id, CurveCommand command) {
        bind(id, [command](const CommandArgs&) { ui::CurveWindow::get().run(command); },
             [command]() { return ui::CurveWindow::get().canRun(command); },
             []() { return !ImGui::GetIO().WantTextInput; });
    };
    curve("curve.frame", CurveCommand::Frame);
    curve("curve.delete", CurveCommand::Delete);
    curve("curve.copy", CurveCommand::Copy);
    curve("curve.paste", CurveCommand::Paste);
    bind("colorPicker.eyedropper", [](const CommandArgs&) { ui::ColorPickerWindow::get().eyedropper(); },
         []() { return ui::ColorPickerWindow::get().canEyedropper(); });

    registerPanelClaims();

    ShortcutOverrides::load(ShortcutOverrides::defaultFile(), catalog);
}

void editor::App::registerPanelClaims(){
    PanelClaims& claims = PanelClaims::editor();
    // (the palette's own field does not count: its entries show what they would do)
    claims.setTyping([this]() { return ImGui::GetIO().WantTextInput && !searchPalette->isOpen(); });
    auto own = [](const char* command) {
        ClaimHandler handler;
        handler.command = command;
        return handler;
    };
    using Verb = EditVerb;
    claims.add({"Curve", []() { return ui::CurveWindow::get().isFocused(); },
                {{Verb::Frame, own("curve.frame")}, {Verb::Delete, own("curve.delete")},
                 {Verb::Copy, own("curve.copy")}, {Verb::Paste, own("curve.paste")}}});
    claims.add({"Color Picker", []() { return ui::ColorPickerWindow::get().isFocused(); }, {}});
    claims.add({"Gradient Editor", []() { return ui::GradientWindow::get().isFocused(); }, {}});
    // its selected map, action or binding
    claims.add({"Input Actions", [this]() { return inputActionsWindow->isFocused(); },
                {{Verb::Delete, {{}, [this]() { return inputActionsWindow->canDelete(); }, [this]() { inputActionsWindow->requestDelete(); }}}}});
    // its selected state, sub-state machine, transition or layer
    claims.add({"Animator", [this]() { return animatorWindow->isFocused(); },
                {{Verb::Delete, {{}, [this]() { return animatorWindow->canDelete(); }, [this]() { animatorWindow->removeSelection(); }}}}});
    claims.add({"Jobs & Memory", [this]() { return jobsMemoryWindow->isFocused(); }, {}});
    claims.add({"Project", [this]() { return projectWindow->isFocused(); },
                {{Verb::Delete, own("project.delete")}, {Verb::Copy, own("project.copy")}, {Verb::Cut, own("project.cut")},
                 {Verb::Paste, own("project.paste")}, {Verb::SelectAll, own("project.selectAll")}, {Verb::Rename, own("project.rename")}}});
    claims.add({"Console", [this]() { return consoleWindow->isListFocused(); }, {{Verb::Copy, own("console.copy")}}});
    claims.add({"Sprite Normal Map", [this]() { return spriteNormalsWindow->isFocused(); },
                {{Verb::Frame, {{}, nullptr, [this]() { spriteNormalsWindow->requestFit(); }}}}});
    // the Animation window editing a clip: the clip's keys
    claims.add({"Animation", [this]() { return clipWindowTakesKeys(); },
                {{Verb::Delete, own("animation.delete")}, {Verb::Duplicate, own("animation.duplicate")}, {Verb::Copy, own("animation.copy")},
                 {Verb::Paste, own("animation.paste")}, {Verb::Frame, own("animation.frameSelected")}}});
    // the Pose Animation window, or a Scene view posing for it (also while its camera flies:
    // the pose commands wait then, and the scene's keys still stay off)
    claims.add({"Pose Animation", [this]() { return poseAnimationWindow->isFocused() || lastFocusedWindow == LastFocusedWindow::PoseAnimation; },
                {{Verb::Delete, own("pose.delete")}, {Verb::Duplicate, own("pose.duplicate")}, {Verb::Copy, own("pose.copy")},
                 {Verb::Paste, own("pose.paste")}, {Verb::Rename, own("pose.rename")}}});
    claims.bindEditCommands();
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
    // the entity mode has the Animation context's keys too while it has focus (S40 R22: Align on an
    // audio block); the edit verbs (Delete, Copy...) stay its own
    const AnimationWindowClip* animationClip = animationWindow ? animationWindow->getClipMode() : nullptr;
    if (clipWindowTakesKeys() || (animationClip && animationClip->entityKeys)) state.panels.push_back(ShortcutContext::Animation);
    if (poseKeysActive()) state.panels.push_back(ShortcutContext::PoseAnimation);
    if (ui::CurveWindow::get().isFocused()) state.panels.push_back(ShortcutContext::Curve);
    if (ui::ColorPickerWindow::get().isFocused()) state.panels.push_back(ShortcutContext::ColorPicker);
    if (soundBrowserWindow->isFocused()) state.panels.push_back(ShortcutContext::SoundBrowser);
    if (audioCapturesWindow->isFocused()) state.panels.push_back(ShortcutContext::AudioCaptures);
    if (state.nativeMenuOwnsKeys) state.nativeMenuChords = PanelClaims::editor().nativeMenuChords();
    if (spritePreviewWindow->isFocused()) state.panels.push_back(ShortcutContext::SpritePreview);
    return state;
}

void editor::App::processShortcuts(){
    syncScriptCommands();
    shortcutDispatcher.process(ImGuiShortcutKeys(), shortcutState());
    // Esc of a Scene view posing the Pose Animation window's character (a local key; the
    // window runs it itself while focused)
    if (poseKeysFromSceneView()) poseAnimationWindow->handleLocalKeys();
}
