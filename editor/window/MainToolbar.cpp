// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "MainToolbar.h"
#include "render/SceneRender.h"
#include "window/PhysicsPlacementWindow.h"

#include "imgui_internal.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/EditorScripts.h"
#include "shortcuts/CommandCatalog.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    // Unity's play mode tint for the Play/Pause buttons
    const ImVec4 PLAY_ACTIVE = ImVec4(0.24f, 0.45f, 0.70f, 1.00f);
    const ImVec4 PLAY_ACTIVE_HOVERED = ImVec4(0.30f, 0.52f, 0.78f, 1.00f);
    constexpr const char* LAYOUT_BUTTON_LABEL = "Layout " ICON_FA_CARET_DOWN "##MainLayout";

    bool isRunningState(editor::ScenePlayState state) {
        return state == editor::ScenePlayState::PLAYING || state == editor::ScenePlayState::PAUSED;
    }

    // Square-ish icon button with a highlighted "on" state
    bool toolbarButton(const char* label, bool on, ImVec2 size, const ImVec4* onColor = nullptr, const ImVec4* onHovered = nullptr) {
        const ImVec4 active = onColor ? *onColor : editor::Theme::Colors::ButtonActivated;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, active);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, onHovered ? *onHovered : active);
        }
        const bool pressed = ImGui::Button(label, size);
        if (on) ImGui::PopStyleColor(2);
        return pressed;
    }

}

editor::MainToolbar::MainToolbar(Project* project): project(project), scriptBadge(project) {
}

float editor::MainToolbar::toolsStripWidth(float rowHeight) const {
    const auto items = EditorScripts::get().itemsFor("toolbar");
    if (items.empty()) return 0.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = 0.0f;
    for (const EditorMenuItem* item : items) {
        width += (item->iconGlyph ? rowHeight : ImGui::CalcTextSize(item->path.c_str()).x + style.FramePadding.x * 2.0f) + style.ItemSpacing.x;
    }
    return width + style.ItemSpacing.x;
}

void editor::MainToolbar::drawToolsStrip(float rowHeight) {
    const auto items = EditorScripts::get().itemsFor("toolbar");
    if (items.empty()) return;
    for (const EditorMenuItem* item : items) {
        ImGui::SameLine();
        ImGui::PushID(item->id.c_str());
        // a Font Awesome icon by name (icon=magnet), else the label
        const char* glyph = item->iconGlyph;
        const bool pressed = glyph ? ImGui::Button(glyph, ImVec2(rowHeight, rowHeight)) : ImGui::Button(item->path.c_str(), ImVec2(0.0f, rowHeight));
        const std::string tooltip = item->tooltip.empty() ? item->path : item->tooltip;
        ImGui::SetItemTooltip("%s", tooltip.c_str());
        if (pressed) {
            EditorMenuRunContext context;
            context.sceneId = project->getSelectedSceneId();
            for (Entity entity : project->getSelectedEntities(context.sceneId)) context.selection.push_back(entity);
            EditorScripts::get().runFromMenu(item->id, context);
        }
        ImGui::PopID();
    }
}

//=== Play mode ===

bool editor::MainToolbar::isInPlayMode() const {
    return project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
}

bool editor::MainToolbar::isPaused() const {
    const SceneProject* sceneProject = project->getScene(project->getPlaySessionSceneId());
    return sceneProject && sceneProject->playState == ScenePlayState::PAUSED;
}

bool editor::MainToolbar::canTogglePlay() const {
    const uint32_t playSceneId = project->getPlaySessionSceneId();
    if (playSceneId != NULL_PROJECT_SCENE) {
        const SceneProject* sceneProject = project->getScene(playSceneId);
        return sceneProject && sceneProject->playState != ScenePlayState::CANCELLING
            && sceneProject->playState != ScenePlayState::SAVING;
    }
    // In Prefab Mode Play runs the scene Prefab Mode was entered from (Unity plays the open
    // scenes; the stage stays open and editable)
    const SceneProject* selected = project->getScene(project->getSceneBehindPrefabStage(project->getSelectedSceneId()));
    return selected && selected->playState == ScenePlayState::STOPPED && !project->isAnyScenePlaying();
}

bool editor::MainToolbar::canPause() const {
    const SceneProject* sceneProject = project->getScene(project->getPlaySessionSceneId());
    return sceneProject && isRunningState(sceneProject->playState);
}

bool editor::MainToolbar::canStep() const {
    return canPause();
}

void editor::MainToolbar::togglePlay() {
    if (!canTogglePlay()) return;

    const uint32_t playSceneId = project->getPlaySessionSceneId();
    if (playSceneId != NULL_PROJECT_SCENE) {
        project->stop(playSceneId);
    } else if (SceneProject* selected = project->getScene(project->getSceneBehindPrefabStage(project->getSelectedSceneId()))) {
        project->start(selected->id);
    }
}

void editor::MainToolbar::togglePause() {
    const uint32_t playSceneId = project->getPlaySessionSceneId();
    const SceneProject* sceneProject = project->getScene(playSceneId);
    if (!sceneProject) return;

    if (sceneProject->playState == ScenePlayState::PLAYING) {
        project->pause(playSceneId);
    } else if (sceneProject->playState == ScenePlayState::PAUSED) {
        project->resume(playSceneId);
    }
}

void editor::MainToolbar::step() {
    if (!canStep()) return;
    project->step(project->getPlaySessionSceneId());
}

//=== Transform tools ===

bool editor::MainToolbar::toolSupported(const SceneProject& sceneProject, Tool tool) const {
    switch (tool) {
        case Tool::Hand:
            return true;
        case Tool::Move:
        case Tool::Rotate:
        case Tool::Scale:
            return sceneProject.sceneType != SceneType::SCENE_UI;
        case Tool::Rect:
            return sceneProject.sceneType != SceneType::SCENE_3D;
    }
    return false;
}

bool editor::MainToolbar::physicsSupported(const SceneProject& sceneProject) const {
    // 2D scenes are a later phase (Box2D)
    return sceneProject.sceneType == SceneType::SCENE_3D && toolSupported(sceneProject, Tool::Move);
}

void editor::MainToolbar::applyTool(SceneProject& sceneProject, Tool tool) const {
    SceneRender* sceneRender = sceneProject.sceneRender;
    if (!sceneRender) return;

    if (tool == Tool::Hand) {
        sceneRender->enableCursorHand();
    } else {
        // Unity has no separate select tool: every transform tool also selects
        sceneRender->enableCursorPointer();
        if (toolSupported(sceneProject, tool)) {
            ToolsLayer* toolsLayer = sceneRender->getToolsLayer();
            switch (tool) {
                case Tool::Move: toolsLayer->enableTranslateGizmo(); break;
                case Tool::Rotate: toolsLayer->enableRotateGizmo(); break;
                case Tool::Scale: toolsLayer->enableScaleGizmo(); break;
                case Tool::Rect: toolsLayer->enableObject2DGizmo(); break;
                default: break;
            }
        }
    }
    sceneProject.needUpdateRender = true;
}

editor::MainToolbar::Tool editor::MainToolbar::getTool() const {
    const SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->sceneRender) return Tool::Move;

    if (sceneProject->sceneRender->getCursorSelected() == CursorSelected::HAND) {
        return Tool::Hand;
    }
    switch (sceneProject->sceneRender->getToolsLayer()->getGizmoSelected()) {
        case GizmoSelected::TRANSLATE: return Tool::Move;
        case GizmoSelected::ROTATE: return Tool::Rotate;
        case GizmoSelected::SCALE: return Tool::Scale;
        case GizmoSelected::OBJECT2D: return Tool::Rect;
    }
    return Tool::Move;
}

bool editor::MainToolbar::canUseTool(Tool tool) const {
    const SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->sceneRender) return false;
    // Viewing through a camera leaves the editor tools inert
    if (sceneProject->playState == ScenePlayState::STOPPED && sceneProject->sceneRender->isPreviewCameraActive()) return false;
    return toolSupported(*sceneProject, tool);
}

void editor::MainToolbar::setTool(Tool tool) {
    if (!canUseTool(tool)) return;
    // Unity's tool is global, so every open Scene view follows it (Prefab Mode's too)
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->opened) applyTool(*sceneProject, tool);
    }
    // Hand and Rect take no physics: a running gesture finishes (and commits)
    Backend::getApp().getPhysicsPlacementWindow()->toolChanged(tool == Tool::Move || tool == Tool::Rotate || tool == Tool::Scale);
}

bool editor::MainToolbar::isGlobalTransform() const {
    const SceneProject* sceneProject = project->getSelectedScene();
    return !sceneProject || !sceneProject->sceneRender || sceneProject->sceneRender->isUseGlobalTransform();
}

void editor::MainToolbar::toggleGlobalTransform() {
    const bool global = !isGlobalTransform();
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->opened && sceneProject->sceneRender) {
            sceneProject->sceneRender->setUseGlobalTransform(global);
            sceneProject->needUpdateRender = true;
        }
    }
}

bool editor::MainToolbar::isPhysicsMode() const {
    return physicsMode;
}

bool editor::MainToolbar::canUsePhysics() const {
    if (project->isAnyScenePlaying()) return false;
    const SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->sceneRender) return false;
    if (sceneProject->sceneRender->isPreviewCameraActive()) return false;
    return physicsSupported(*sceneProject);
}

void editor::MainToolbar::setPhysicsMode(bool on) {
    // off is always allowed, on only where physics works
    if (on == physicsMode || (on && !canUsePhysics())) return;
    physicsMode = on;
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->opened) sceneProject->needUpdateRender = true;
    }
    Backend::getApp().getPhysicsPlacementWindow()->physicsModeChanged(on);
}

void editor::MainToolbar::togglePhysicsMode() {
    setPhysicsMode(!physicsMode);
}

//=== Play mode keys (F5 / F6 / F7) ===

void editor::MainToolbar::playOrResume() {
    if (!isInPlayMode()) {
        togglePlay();
    } else if (isPaused()) {
        togglePause();
    }
}

bool editor::MainToolbar::canPlayOrResume() const {
    return isInPlayMode() ? isPaused() : canTogglePlay();
}

void editor::MainToolbar::pauseOnly() {
    if (canPause() && !isPaused()) togglePause();
}

bool editor::MainToolbar::canStopSession() const {
    const SceneProject* sceneProject = project->getScene(project->getPlaySessionSceneId());
    return sceneProject && (isRunningState(sceneProject->playState) || sceneProject->playState == ScenePlayState::LOADING);
}

void editor::MainToolbar::stopSession() {
    if (canStopSession()) project->stop(project->getPlaySessionSceneId());
}

//=== Drawing ===

void editor::MainToolbar::drawTools(float rowHeight) {
    const ImVec2 buttonSize(Theme::dpi(30.0f), rowHeight);
    const Tool current = getTool();

    struct ToolButton {
        Tool tool;
        const char* icon;
        const char* command;    // its catalog command: the tooltip's name and shortcut
    };
    const ToolButton buttons[] = {
        { Tool::Hand, ICON_FA_HAND "##ToolHand", "tools.hand" },
        { Tool::Move, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "##ToolMove", "tools.move" },
        { Tool::Rotate, ICON_FA_ROTATE "##ToolRotate", "tools.rotate" },
        { Tool::Scale, ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "##ToolScale", "tools.scale" },
        { Tool::Rect, ICON_FA_VECTOR_SQUARE "##ToolRect", "tools.rect" },
    };
    const CommandCatalog& catalog = CommandCatalog::editor();
    auto commandTooltip = [&catalog](const char* id) {
        const CommandDef* def = catalog.find(id);
        if (!def) return std::string(id);
        std::string text = catalog.tooltip(id, def->name);
        if (!def->description.empty()) text += "\n" + def->description;
        return text;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::dpi(1.0f), 0.0f));
    for (size_t i = 0; i < IM_ARRAYSIZE(buttons); i++) {
        if (i > 0) ImGui::SameLine();
        const ToolButton& button = buttons[i];
        ImGui::BeginDisabled(!canUseTool(button.tool));
        if (toolbarButton(button.icon, current == button.tool, buttonSize)) {
            setTool(button.tool);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", commandTooltip(button.command).c_str());
    }
    ImGui::PopStyleVar();

    // Physics (G), a modifier on the transform tools, and its panel
    ImGui::SameLine(0, Theme::dpi(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::dpi(1.0f), 0.0f));
    // where physics can't work (2D, camera preview, Play) it can still be turned off
    ImGui::BeginDisabled(!physicsMode && !canUsePhysics());
    if (toolbarButton(ICON_FA_MAGNET "##ToolPhysics", physicsMode, buttonSize, &Theme::Colors::PhysicsAccent, &Theme::Colors::PhysicsAccentHovered)) {
        togglePhysicsMode();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Physics (G)\nMove, Rotate and Scale push the selection through a physics\n"
                          "simulation: it slides along and stops at what it touches, then settles.\n"
                          "Hold Space to drop, Shift-drag a selected object to snap, Esc to cancel.\n"
                          "3D scenes, not in Play mode.");
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    if (ImGui::Button(ICON_FA_CARET_DOWN "##PhysicsPanel", ImVec2(Theme::dpi(16.0f), rowHeight))) {
        Backend::getApp().getPhysicsPlacementWindow()->open();
    }
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("Physics Placement settings");
    ImGui::PopStyleVar();

    // Global/Local handle orientation (Unity: X)
    ImGui::SameLine(0, Theme::dpi(10.0f));
    const SceneProject* sceneProject = project->getSelectedScene();
    const bool rectTool = current == Tool::Rect;
    const bool multiple = sceneProject && sceneProject->sceneRender && sceneProject->sceneRender->isMultipleEntitesSelected();
    const bool global = isGlobalTransform();
    const char* label = global ? ICON_FA_GLOBE " Global##ToolSpace" : ICON_FA_CUBE " Local##ToolSpace";
    ImGui::BeginDisabled(!sceneProject || rectTool || multiple || !canUseTool(Tool::Hand));
    if (ImGui::Button(label, ImVec2(Theme::dpi(78.0f), rowHeight))) {
        toggleGlobalTransform();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", commandTooltip("tools.toggleGlobalLocal").c_str());
}

void editor::MainToolbar::drawPlayControls(float rowHeight) {
    const ImVec2 buttonSize(Theme::dpi(34.0f), rowHeight);
    const bool inPlayMode = isInPlayMode();
    const bool paused = isPaused();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::dpi(1.0f), 0.0f));

    ImGui::BeginDisabled(!canTogglePlay());
    if (toolbarButton(ICON_FA_PLAY "##MainPlay", inPlayMode, buttonSize, &PLAY_ACTIVE, &PLAY_ACTIVE_HOVERED)) {
        togglePlay();
    }
    ImGui::EndDisabled();
    const CommandCatalog& catalog = CommandCatalog::editor();
    ImGui::SetItemTooltip("%s", catalog.tooltip("edit.play", inPlayMode ? "Stop" : "Play").c_str());

    ImGui::SameLine();
    ImGui::BeginDisabled(!canPause());
    if (toolbarButton(ICON_FA_PAUSE "##MainPause", paused, buttonSize, &PLAY_ACTIVE, &PLAY_ACTIVE_HOVERED)) {
        togglePause();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", catalog.tooltip("edit.pause", paused ? "Resume" : "Pause").c_str());

    ImGui::SameLine();
    ImGui::BeginDisabled(!canStep());
    if (toolbarButton(ICON_FA_FORWARD_STEP "##MainStep", false, buttonSize)) {
        step();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", catalog.tooltip("edit.step", "Step one frame").c_str());

    // the Recorder's record button (S27 R26): starts Play when needed, F10 too
    ImGui::SameLine();
    const bool recording = Backend::getApp().isRecording();
    static const ImVec4 RECORD_ACTIVE(0.75f, 0.12f, 0.12f, 1.0f);
    static const ImVec4 RECORD_ACTIVE_HOVERED(0.85f, 0.2f, 0.2f, 1.0f);
    if (toolbarButton(ICON_FA_CIRCLE "##MainRecord", recording, buttonSize, &RECORD_ACTIVE, &RECORD_ACTIVE_HOVERED)) {
        Backend::getApp().toggleRecording();
    }
    ImGui::SetItemTooltip("%s", catalog.tooltip("recorder.toggle", recording ? "Stop Recording" : "Start Recording").c_str());

    ImGui::PopStyleVar();
}

void editor::MainToolbar::drawLayoutMenu(float rowHeight) {
    if (ImGui::Button(LAYOUT_BUTTON_LABEL, ImVec2(0.0f, rowHeight))) {
        ImGui::OpenPopup("MainLayoutPopup");
    }
    ImGui::SetItemTooltip("Window layout");

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup("MainLayoutPopup")) {
        // the same items as Window > Layouts: the presets, then Save / Rename / Delete / files / Reset
        Backend::getApp().drawLayoutsMenu();
        ImGui::EndPopup();
    }
}

void editor::MainToolbar::show() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;

    const float rowHeight = ImGui::GetFrameHeight() + Theme::dpi(2.0f);
    const ImVec2 padding = Theme::dpi(ImVec2(8.0f, 5.0f));
    const float barHeight = rowHeight + padding.y * 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg).w > 0.0f
        ? ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg)
        : ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const bool open = ImGui::BeginViewportSideBar("##MainToolbar", viewport, ImGuiDir_Up, barHeight, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    if (open) {
        const float startY = ImGui::GetCursorPosY();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::dpi(3.0f));

        drawTools(rowHeight);

        // Centre: Play / Pause / Step
        const ImGuiStyle& style = ImGui::GetStyle();
        const float playWidth = Theme::dpi(34.0f) * 3.0f + Theme::dpi(1.0f) * 2.0f;
        const float centreX = (ImGui::GetWindowWidth() - playWidth) * 0.5f;
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(std::max(centreX, ImGui::GetCursorPosX() + style.ItemSpacing.x), startY));
        drawPlayControls(rowHeight);

        // the scripts' compile and reload state, right of the play controls (S23 R40)
        ImGui::SameLine(0.0f, Theme::dpi(12.0f));
        ImGui::SetCursorPosY(startY);
        scriptBadge.draw(rowHeight);

        // Right: Layout, then the editor scripts' Tools strip
        const float layoutWidth = ImGui::CalcTextSize(LAYOUT_BUTTON_LABEL, nullptr, true).x + style.FramePadding.x * 2.0f;
        const float rightX = ImGui::GetWindowContentRegionMax().x - layoutWidth - toolsStripWidth(rowHeight);
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(std::max(rightX, ImGui::GetCursorPosX() + style.ItemSpacing.x), startY));
        drawLayoutMenu(rowHeight);
        drawToolsStrip(rowHeight);

        ImGui::PopStyleVar();
    }
    ImGui::End();
}
