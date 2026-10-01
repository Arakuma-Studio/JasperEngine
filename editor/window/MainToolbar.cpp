// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "MainToolbar.h"

#include "imgui_internal.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"

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

editor::MainToolbar::MainToolbar(Project* project): project(project) {
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
    const SceneProject* selected = project->getSelectedScene();
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
    } else if (SceneProject* selected = project->getSelectedScene()) {
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
    // Unity's tool is global, so every open Scene view follows it
    for (SceneProject& sceneProject : project->getScenes()) {
        if (sceneProject.opened) applyTool(sceneProject, tool);
    }
}

bool editor::MainToolbar::isGlobalTransform() const {
    const SceneProject* sceneProject = project->getSelectedScene();
    return !sceneProject || !sceneProject->sceneRender || sceneProject->sceneRender->isUseGlobalTransform();
}

void editor::MainToolbar::toggleGlobalTransform() {
    const bool global = !isGlobalTransform();
    for (SceneProject& sceneProject : project->getScenes()) {
        if (sceneProject.opened && sceneProject.sceneRender) {
            sceneProject.sceneRender->setUseGlobalTransform(global);
            sceneProject.needUpdateRender = true;
        }
    }
}

//=== Shortcuts ===

void editor::MainToolbar::handleShortcuts(bool toolShortcuts) {
    ImGuiIO& io = ImGui::GetIO();

    // Play mode (Unity: Ctrl+P, Ctrl+Shift+P, Ctrl+Alt+P), plus F5/F6/F7
    if (io.KeyCtrl && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (io.KeyAlt) {
            step();
        } else if (io.KeyShift) {
            togglePause();
        } else {
            togglePlay();
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (!isInPlayMode()) {
            togglePlay();
        } else if (isPaused()) {
            togglePause();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && canPause() && !isPaused()) {
        togglePause();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
        const SceneProject* sceneProject = project->getScene(project->getPlaySessionSceneId());
        if (sceneProject && (isRunningState(sceneProject->playState) || sceneProject->playState == ScenePlayState::LOADING)) {
            project->stop(sceneProject->id);
        }
    }

    // Tools: not while typing, flying the 3D camera (RMB + WASDQE) or playing
    // with the keyboard going to the game (the Scene view stays the editor's)
    if (!toolShortcuts || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsAnyItemActive()) return;
    if (project->isAnyScenePlaying() && Backend::getApp().getGameWindow()->hasKeyboardFocus()) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) setTool(Tool::Hand);
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) setTool(Tool::Move);
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) setTool(Tool::Rotate);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) setTool(Tool::Scale);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) setTool(Tool::Rect);
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) toggleGlobalTransform();
}

//=== Drawing ===

void editor::MainToolbar::drawTools(float rowHeight) {
    const ImVec2 buttonSize(Theme::dpi(30.0f), rowHeight);
    const Tool current = getTool();

    struct ToolButton {
        Tool tool;
        const char* icon;
        const char* tooltip;
    };
    const ToolButton buttons[] = {
        { Tool::Hand, ICON_FA_HAND "##ToolHand", "Hand Tool (Q)\nPan the Scene view" },
        { Tool::Move, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "##ToolMove", "Move Tool (W)" },
        { Tool::Rotate, ICON_FA_ROTATE "##ToolRotate", "Rotate Tool (E)" },
        { Tool::Scale, ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER "##ToolScale", "Scale Tool (R)" },
        { Tool::Rect, ICON_FA_VECTOR_SQUARE "##ToolRect", "Rect Tool (T)\n2D and UI scenes" },
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
        ImGui::SetItemTooltip("%s", button.tooltip);
    }
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
    ImGui::SetItemTooltip("Toggle tool handle rotation between Global and Local (X)");
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
    ImGui::SetItemTooltip(inPlayMode ? "Stop (Ctrl+P)" : "Play (Ctrl+P)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!canPause());
    if (toolbarButton(ICON_FA_PAUSE "##MainPause", paused, buttonSize, &PLAY_ACTIVE, &PLAY_ACTIVE_HOVERED)) {
        togglePause();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(paused ? "Resume (Ctrl+Shift+P)" : "Pause (Ctrl+Shift+P)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!canStep());
    if (toolbarButton(ICON_FA_FORWARD_STEP "##MainStep", false, buttonSize)) {
        step();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Step one frame (Ctrl+Alt+P)");

    ImGui::PopStyleVar();
}

void editor::MainToolbar::drawLayoutMenu(float rowHeight) {
    if (ImGui::Button(LAYOUT_BUTTON_LABEL, ImVec2(0.0f, rowHeight))) {
        ImGui::OpenPopup("MainLayoutPopup");
    }
    ImGui::SetItemTooltip("Window layout");

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup("MainLayoutPopup")) {
        if (ImGui::MenuItem("Reset Layout")) {
            Backend::getApp().resetLayout();
        }
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

        // Right: Layout
        const float layoutWidth = ImGui::CalcTextSize(LAYOUT_BUTTON_LABEL, nullptr, true).x + style.FramePadding.x * 2.0f;
        const float rightX = ImGui::GetWindowContentRegionMax().x - layoutWidth;
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(std::max(rightX, ImGui::GetCursorPosX() + style.ItemSpacing.x), startY));
        drawLayoutMenu(rowHeight);

        ImGui::PopStyleVar();
    }
    ImGui::End();
}
