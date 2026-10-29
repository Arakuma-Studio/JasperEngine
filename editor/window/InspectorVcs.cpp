// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The baseline tooltip, "Revert to Baseline" and "Copy Baseline Value" follow OpenEngine's
// InspectorVcsController (Apps/Editor/Source/VersionControl/Ui/InspectorVcsController.cpp:418-499,
// MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md), drawn as an
// ImGui gutter marker.

// The Inspector's version control (docs/specs/S29-version-control.md R28): the lock banner of a
// lockable asset or open document ("Locked by <owner> - read only" with Edit Anyway, "Not
// locked" with Lock, "Locked by you" with Unlock), and the scene diff's markers: a 2-px amber bar
// at the right edge of every property row that differs from the baseline (the prefab override
// bar keeps the left edge), its baseline value as a tooltip, Revert to Baseline and Copy
// Baseline Value on its context menu, Revert Component to Baseline on the component's.

#include "InspectorWindow.h"

#include "AppSettings.h"
#include "Backend.h"
#include "App.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/RevertToBaselineCmd.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Reflect.h"
#include "vcs/SceneDiffTracker.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsUi.h"

using namespace jasperengine;
namespace scene = jasperengine::editor::vcs::scene;

namespace {

    // a diff path and an Inspector row id name the same value (one inside the other)
    bool pathsMeet(const std::string& diffPath, const std::string& rowId) {
        if (diffPath == rowId) return true;
        auto inside = [](const std::string& outer, const std::string& inner) {
            return inner.size() > outer.size() && inner.compare(0, outer.size(), outer) == 0 &&
                   (inner[outer.size()] == '.' || inner[outer.size()] == '[');
        };
        return inside(diffPath, rowId) || inside(rowId, diffPath);
    }

}

std::string editor::InspectorWindow::vcsDiffPathOfRow(const scene::ComponentDiff& component, const std::string& rowId) const {
    // a script property's row ("scripts[1].speed") is keyed in the diff by its script
    // ("scripts[lua:Enemy.lua].properties[speed]")
    if (component.key != "script" || rowId.rfind("scripts[", 0) != 0) return rowId;
    const size_t close = rowId.find(']');
    if (close == std::string::npos || close + 2 > rowId.size() || rowId[close + 1] != '.') return rowId;
    const size_t index = static_cast<size_t>(std::atoi(rowId.substr(8, close - 8).c_str()));
    const YAML::Node scripts = component.currentNode.IsDefined() && component.currentNode.IsMap() ? component.currentNode["scripts"] : YAML::Node();
    if (!scripts || !scripts.IsSequence() || index >= scripts.size()) return rowId;
    std::string key;
    if (!scene::sequenceItemKey("scripts", scripts[index], key)) return rowId;
    return "scripts[" + key + "].properties[" + rowId.substr(close + 2) + "]";
}

const scene::PropertyDiff* editor::InspectorWindow::findVcsPropertyDiff(SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                                       ComponentType type, const std::string& rowId,
                                                                       const scene::ObjectDiff** objectOut, const scene::ComponentDiff** componentOut) const {
    if (!sceneProject || entities.size() != 1 || rowId.empty()) return nullptr;
    const vcs::SceneDiffView* view = project->getSceneDiffs().view(sceneProject->id);
    if (!view) return nullptr;
    const scene::ObjectDiff* object = view->object(entities[0]);
    if (!object) return nullptr;
    const reflect::ComponentDesc* desc = reflect::find(type);
    if (!desc) return nullptr;
    const scene::ComponentDiff* component = object->component(desc->key);
    if (!component) return nullptr;
    const std::string path = vcsDiffPathOfRow(*component, rowId);
    for (const scene::PropertyDiff& property : component->properties) {
        if (pathsMeet(property.path, path)) {
            if (objectOut) *objectOut = object;
            if (componentOut) *componentOut = component;
            return &property;
        }
    }
    return nullptr;
}

void editor::InspectorWindow::drawVcsBaselineItems(SceneProject* sceneProject, const scene::ObjectDiff& object,
                                                   const scene::ComponentDiff* component, const scene::PropertyDiff* property, bool readOnly) {
    const vcs::SceneDiffView* view = project->getSceneDiffs().view(sceneProject->id);
    if (!view || !view->baseline) return;
    const std::string label = property ? "Revert to Baseline" : component ? "Revert Component to Baseline" : "Revert GameObject to Baseline";
    std::string why;
    std::unique_ptr<RevertToBaselineCmd> command(readOnly ? nullptr :
        RevertToBaselineCmd::build(project, sceneProject->id, *view->baseline, object, component, property, &why));
    if (readOnly) why = "The Inspector is read only";
    if (ImGui::MenuItem((ICON_FA_ROTATE_LEFT "  " + label).c_str(), nullptr, false, command != nullptr)) {
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(command.release());
    }
    if (!command && !why.empty()) ImGui::SetItemTooltip("%s", why.c_str());
    if (property) {
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy Baseline Value", nullptr, false, property->inBase)) {
            ImGui::SetClipboardText(property->baseValue.c_str());
        }
        if (!property->inBase) ImGui::SetItemTooltip("The baseline has no value here (the property is new)");
    }
}

bool editor::InspectorWindow::drawVcsPropertyMarker(const std::string& rowId, const ImVec2& cellMin, const ImVec2& cellMax, bool openMenu) {
    const scene::ObjectDiff* object = nullptr;
    const scene::ComponentDiff* component = nullptr;
    const scene::PropertyDiff* property = findVcsPropertyDiff(prefabRow.sceneProject, prefabRow.entities, prefabRow.type, rowId, &object, &component);
    if (!property) return false;
    // the right edge of the window, the prefab override bar keeping the left
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    drawList->PushClipRect(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), false);
    const float x1 = windowPos.x + windowSize.x - Theme::dpi(1.0f);
    drawList->AddRectFilled(ImVec2(x1 - Theme::dpi(2.0f), cellMin.y), ImVec2(x1, cellMax.y), IM_COL32(240, 176, 40, 255));
    drawList->PopClipRect();
    const std::string popupId = "##vcs_property_menu_" + rowId;
    const ImVec2 rowMax(windowPos.x + windowSize.x, cellMax.y);
    if (ImGui::IsMouseHoveringRect(cellMin, rowMax) && !ImGui::IsPopupOpen(popupId.c_str())) {
        ImGui::SetItemTooltip("Baseline: %s", property->inBase ? property->baseValue.c_str() : "(none: new since the baseline)");
    }
    if (openMenu && ImGui::IsMouseHoveringRect(cellMin, cellMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        ImGui::OpenPopup(popupId.c_str());
    }
    ImGui::BeginDisabled(false);
    if (ImGui::BeginPopup(popupId.c_str())) {
        ImGui::TextDisabled("%s", scene::propertyPath(*component, *property).c_str());
        drawVcsBaselineItems(prefabRow.sceneProject, *object, component, property, prefabRow.readOnly);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    return true;
}

void editor::InspectorWindow::drawVcsComponentMenuItems(SceneProject* sceneProject, const std::vector<Entity>& entities, ComponentType type, bool readOnly) {
    if (!sceneProject || entities.size() != 1) return;
    const vcs::SceneDiffView* view = project->getSceneDiffs().view(sceneProject->id);
    const reflect::ComponentDesc* desc = reflect::find(type);
    if (!view || !desc) return;
    const scene::ObjectDiff* object = view->object(entities[0]);
    const scene::ComponentDiff* component = object ? object->component(desc->key) : nullptr;
    if (!component) return;
    ImGui::Separator();
    drawVcsBaselineItems(sceneProject, *object, component, nullptr, readOnly);
}

bool editor::InspectorWindow::drawVcsLockBanner(const std::vector<std::filesystem::path>& files) {
    vcs::VcsUi* ui = Backend::getApp().getVcsUi();
    return ui ? ui->drawLockBanner(files) : false;
}
