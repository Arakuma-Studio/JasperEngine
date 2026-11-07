// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Button's Inspector (ui-components spec 6.8): Unity's Interactable on top, the reflected
// Transition and Navigation rows (shown by mode), Navigation's Visualize, and the state in Play.

#include "InspectorWindow.h"
#include "component/ButtonComponent.h"
#include "subsystem/UISystem.h"
#include "AppSettings.h"
#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"

using namespace jasperengine;

namespace {

    const char* selectionStateName(jasperengine::SelectionState state){
        switch (state){
            case jasperengine::SelectionState::HIGHLIGHTED: return "Highlighted";
            case jasperengine::SelectionState::PRESSED: return "Pressed";
            case jasperengine::SelectionState::SELECTED: return "Selected";
            case jasperengine::SelectionState::DISABLED: return "Disabled";
            default: return "Normal";
        }
    }

}

void editor::InspectorWindow::drawButtonComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    // Unity's Interactable: the stored flag is `disabled`, so the box shows its inverse
    beginTable(cpType, getLabelSize("Interactable"));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Interactable");
    ImGui::SetItemTooltip("Off: the button shows its Disabled state and takes no press, click or Submit");
    ImGui::TableSetColumnIndex(1);
    // (get_inspector_layout: the row edits `disabled`, shown inverted)
    recordLayoutRow(RowPropertyType::Bool, cpType, "disabled", "Interactable", sceneProject, entities, true);
    bool all = true, any = false;
    for (const Entity& entity : entities){
        if (const ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)){
            const bool interactable = !button->disabled;
            all = all && interactable;
            any = any || interactable;
        }
    }
    bool value = all;
    if (any && !all) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    if (ImGui::Checkbox("##button_interactable", &value)){
        for (const Entity& entity : entities){
            cmd = new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, "disabled", !value);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        if (cmd) cmd->setNoMerge();
    }
    if (any && !all) ImGui::PopStyleColor();
    endTable();

    drawDefaultComponent(cpType, sceneProject, entities);

    // Unity's Navigation > Visualize: the arrows between the selectables in the Scene view (an
    // editor preference, not part of the scene)
    bool visualize = AppSettings::getUINavigationVisualize();
    if (visualize) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " Visualize##button_navigation")){
        AppSettings::setUINavigationVisualize(!visualize);
    }
    if (visualize) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Draw where the arrow keys, the d-pad and a TV remote move the selection, in the Scene view");

    // the live state while playing
    if (entities.size() == 1 && project->isAnyScenePlaying()){
        if (scene->findComponent<ButtonComponent>(entities[0])){
            const jasperengine::SelectionState state = scene->getSystem<UISystem>()->getButtonState(entities[0]);
            const bool selected = scene->getSystem<UISystem>()->getSelected() == entities[0];
            ImGui::TextDisabled("State: %s%s", selectionStateName(state), selected ? " (the scene's selection)" : "");
        }
    }
}
