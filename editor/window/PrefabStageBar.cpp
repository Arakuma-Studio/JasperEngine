// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab Mode's bar over its Scene view (Unity's breadcrumbs and Prefab Mode header): the
// stages from the scenes to the shown one, Save, Auto Save and the Context visibility

#include "SceneWindow.h"

#include "EditorHost.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/BidiText.h"
#include "util/UIUtils.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    // A breadcrumb: flat text-like button, the current one plain and not clickable
    bool crumb(const std::string& label, bool current, const ImVec4& color) {
        if (current) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color, "%s", label.c_str());
            return false;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.x, color.y, color.z, color.w * 0.8f));
        const bool pressed = ImGui::Button(label.c_str());
        ImGui::PopStyleColor(2);
        return pressed;
    }

}

std::string editor::SceneWindow::getPrefabStageWindowTitle(const SceneProject& sceneProject) const {
    // one tab for Prefab Mode, named after the prefab it shows
    return std::string(ICON_FA_CUBE "  ") + BidiText::toVisual(sceneProject.name)
        + (project->hasUnsavedPrefabStage(sceneProject.id) ? " *" : "") + PREFAB_STAGE_WINDOW_ID;
}

void editor::SceneWindow::requestExitPrefabStages(size_t depth) {
    // Deferred: the stage being drawn may be the one closing
    Project* proj = project;
    editor::getEditorHost().enqueueMainThreadTask([proj, depth]() {
        proj->closePrefabStages(depth);
    });
}

void editor::SceneWindow::drawPrefabStageBar(SceneProject& sceneProject) {
    PrefabStage* stage = project->getPrefabStage(sceneProject.id);
    if (!stage) {
        return;
    }
    const size_t count = project->getPrefabStageCount();
    const size_t depth = project->getPrefabStageDepth(sceneProject.id);

    ImGui::PushID("PrefabStageBar");
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y;
    ImGui::GetWindowDrawList()->AddRectFilled(barMin,
        ImVec2(barMin.x + ImGui::GetContentRegionAvail().x, barMin.y + barHeight),
        ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Back: one level up (the stage below, or the scenes)
    if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
        requestExitPrefabStages(depth > 0 ? depth - 1 : 0);
    }
    if (depth > 1) {
        const PrefabStage* below = project->getPrefabStageAt(depth - 2);
        ImGui::SetItemTooltip("Back to %s", below ? below->sceneProject.name.c_str() : "the previous prefab");
    } else {
        ImGui::SetItemTooltip("Back to the scene (exits Prefab Mode)");
    }

    // Breadcrumbs: Scenes > Outer > Inner
    const ImVec4 textColor = ImGui::GetStyle().Colors[ImGuiCol_Text];
    ImGui::SameLine(0, Theme::dpi(6.0f));
    const SceneProject* behind = project->getScene(project->getSceneBehindPrefabStage(sceneProject.id));
    if (crumb(std::string(ICON_FA_CUBES "  Scenes"), false, textColor)) {
        requestExitPrefabStages(0);
    }
    ImGui::SetItemTooltip("Exit Prefab Mode%s%s", behind ? ", back to " : "", behind ? behind->name.c_str() : "");
    for (size_t i = 0; i < count; i++) {
        const PrefabStage* crumbStage = project->getPrefabStageAt(i);
        if (!crumbStage) {
            continue;
        }
        ImGui::SameLine(0, Theme::dpi(2.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
        ImGui::SameLine(0, Theme::dpi(2.0f));
        const bool current = i + 1 == count;
        std::string label = std::string(ICON_FA_CUBE "  ") + BidiText::toVisual(crumbStage->sceneProject.name);
        if (project->hasUnsavedPrefabStage(crumbStage->sceneProject.id)) {
            label += " *";
        }
        ImGui::PushID(static_cast<int>(i));
        if (crumb(label, current, Theme::Colors::PrefabText)) {
            requestExitPrefabStages(i + 1);
        }
        ImGui::PopID();
        // a Prefab Variant: Unity's variant icon, its base in the tooltip
        const std::filesystem::path base = project->getPrefabFileBase(crumbStage->assetPath);
        if (!base.empty()) {
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const float fontSize = ImGui::GetFontSize();
            const float iconX = itemMin.x + (current ? 0.0f : ImGui::GetStyle().FramePadding.x);
            const float iconY = itemMin.y + (ImGui::GetItemRectSize().y - fontSize) * 0.5f;
            UIUtils::drawPrefabVariantBadge(ImGui::GetWindowDrawList(), ImVec2(iconX, iconY),
                ImVec2(iconX + ImGui::CalcTextSize(ICON_FA_CUBE).x, iconY + fontSize), ImGui::GetColorU32(Theme::Colors::PrefabText));
        }
        ImGui::SetItemTooltip("%s%s%s%s", crumbStage->assetPath.generic_string().c_str(),
            base.empty() ? "" : "\nPrefab Variant of ", base.empty() ? "" : base.generic_string().c_str(),
            crumbStage->inContext() ? "\nIn Context" : "");
    }

    // Right side: Save (Auto Save off), Auto Save, Context
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool autoSave = project->isPrefabAutoSave();
    const bool unsaved = project->hasUnsavedPrefabStage(sceneProject.id);
    const char* visibilityNames[] = {"Normal", "Gray", "Hidden"};
    const int visibility = static_cast<int>(project->getPrefabContextVisibility());
    const std::string contextLabel = std::string("Context: ") + visibilityNames[visibility] + " " ICON_FA_CARET_DOWN;
    // Auto Save never writes over an asset that did not load completely
    const bool degraded = project->isPrefabStageDegraded(sceneProject.id);
    const char* pausedText = "Auto Save paused: this prefab did not load completely";
    const bool showSave = !autoSave || degraded;
    const float saveWidth = (showSave ? ImGui::CalcTextSize("Save").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f) +
        (autoSave && degraded ? ImGui::CalcTextSize(pausedText).x + style.ItemSpacing.x : 0.0f);
    const float autoSaveWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto Save").x + style.ItemSpacing.x;
    const float contextWidth = stage->inContext() ? ImGui::CalcTextSize(contextLabel.c_str(), nullptr, true).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f;
    const float rightX = ImGui::GetWindowContentRegionMax().x - (saveWidth + autoSaveWidth + contextWidth) + style.ItemSpacing.x;
    ImGui::SameLine();
    if (rightX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(rightX);
    }

    if (autoSave && degraded) {
        ImGui::TextColored(Theme::Colors::WarningText, "%s", pausedText);
        ImGui::SetItemTooltip("Saving it would remove values this editor could not read. Save asks first.");
        ImGui::SameLine();
    }
    if (showSave) {
        ImGui::BeginDisabled(!unsaved);
        if (ImGui::Button("Save")) {
            project->savePrefabStage(sceneProject.id);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Save the prefab asset (Ctrl+S)");
        ImGui::SameLine();
    }

    bool autoSaveValue = autoSave;
    if (ImGui::Checkbox("Auto Save", &autoSaveValue)) {
        project->setPrefabAutoSave(autoSaveValue);
    }
    ImGui::SetItemTooltip("Save the prefab asset after each change made in Prefab Mode");

    if (stage->inContext()) {
        ImGui::SameLine();
        if (ImGui::Button(contextLabel.c_str())) {
            ImGui::OpenPopup("PrefabContextVisibility");
        }
        ImGui::SetItemTooltip("How the scene the prefab was opened from shows behind it");
        if (ImGui::BeginPopup("PrefabContextVisibility")) {
            for (int i = 0; i < 3; i++) {
                if (ImGui::MenuItem(visibilityNames[i], nullptr, visibility == i)) {
                    project->setPrefabContextVisibility(static_cast<PrefabContextVisibility>(i));
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, style.FramePadding.y * 0.5f));
}
