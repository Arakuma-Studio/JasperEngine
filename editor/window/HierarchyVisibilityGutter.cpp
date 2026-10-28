// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's SceneVisibility gutter (docs/specs/S26-scene-view.md R23): left of each GameObject row
// an eye hides it in the Scene view and a hand locks it against picking there, shown while the
// row is hovered or when set. A click applies to the GameObject and its children, Alt+click to
// it alone. Editor-only (EntityMetadata::editorFlags, workspace.yaml), never undoable. The
// lock toggle follows OpenEngine's HierarchyPanel (Copyright (c) 2026 Gamecrafters Guild Limited
// and contributors, MIT License, NOTICE.md); the widget is ours.

#include "HierarchyWindow.h"

#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "sceneview/SceneViewController.h"
#include "sceneview/SceneViewVisibility.h"

#include "imgui_internal.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

using namespace jasperengine;

float editor::HierarchyWindow::visibilityGutterWidth() const {
    return ImGui::GetFrameHeight() * VISIBILITY_GUTTER_ICONS;
}

void editor::HierarchyWindow::drawVisibilityGutter(const TreeNode& node, float rowMinY, float rowMaxY) {
    if (node.isScene || node.isChildScene || node.isPlayCreated) {
        return;
    }
    SceneProject* sceneProject = project->getScene(getNodeSceneId(node));
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(node.id)) {
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float size = std::max(rowMaxY - rowMinY, 1.0f);
    // the column at the window's left, whatever the row's depth
    const float x0 = window->DC.CursorStartPos.x;
    const bool rowHovered = ImGui::IsWindowHovered() &&
                            ImGui::IsMouseHoveringRect(ImVec2(x0, rowMinY), ImVec2(window->WorkRect.Max.x, rowMaxY), false);
    const bool hidden = sceneview::isHidden(*sceneProject, node.id);
    const bool locked = sceneview::isPickLocked(*sceneProject, node.id);
    const bool alt = ImGui::GetIO().KeyAlt;

    struct Toggle {
        const char* id;
        bool set;
        const char* onIcon;
        const char* offIcon;
        std::string tooltipOn;
        std::string tooltipOff;
    };
    // the keys from the command catalog (Scene View: H, Alt+H, L)
    auto keys = [](std::initializer_list<std::pair<const char*, const char*>> commands) {
        std::string text;
        for (const auto& [id, what] : commands) {
            const std::string binding = sceneViewShortcut(id);
            if (binding.empty()) continue;
            text += (text.empty() ? ". " : ", ") + binding + what;
        }
        return text;
    };
    const Toggle toggles[2] = {
        {"##SceneVisibility", hidden, ICON_FA_EYE_SLASH, ICON_FA_EYE,
         "Hidden in the Scene view (click: show; Alt+click: this GameObject only)",
         "Hide in the Scene view (click: with children; Alt+click: this GameObject only)" +
             keys({{"sceneView.hideSelection", " hides the selection"}, {"sceneView.showAll", " shows all"}})},
        {"##ScenePickLock", locked, ICON_FA_HAND, ICON_FA_HAND_POINTER,
         "Not pickable in the Scene view (click: pickable again; Alt+click: this GameObject only)",
         "Lock against picking in the Scene view (click: with children; Alt+click: this GameObject only)" +
             keys({{"sceneView.togglePickLock", " locks the selection"}})},
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i = 0; i < 2; i++) {
        const Toggle& toggle = toggles[i];
        const ImRect bb(ImVec2(x0 + size * (float)i, rowMinY), ImVec2(x0 + size * (float)(i + 1), rowMaxY));
        const ImGuiID id = window->GetID(toggle.id);
        if (!ImGui::ItemAdd(bb, id)) {
            continue;
        }
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        if (toggle.set || rowHovered || hovered) {
            const char* icon = toggle.set ? toggle.onIcon : toggle.offIcon;
            const ImVec2 textSize = ImGui::CalcTextSize(icon);
            const ImU32 color = ImGui::GetColorU32(toggle.set ? ImGuiCol_Text : (hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            drawList->AddText(ImVec2(bb.Min.x + (size - textSize.x) * 0.5f, bb.Min.y + (size - textSize.y) * 0.5f), color, icon);
        }
        if (hovered) {
            ImGui::SetTooltip("%s", (toggle.set ? toggle.tooltipOn : toggle.tooltipOff).c_str());
        }
        if (pressed) {
            if (i == 0) {
                sceneview::setHidden(*sceneProject, node.id, !hidden, !alt);
            } else {
                sceneview::setPickLocked(*sceneProject, node.id, !locked, !alt);
            }
            project->saveWorkspaceFile();
        }
    }
}
