// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Dialogue Runner's Inspector (docs/specs/dialogue-graphs.md R24): the table's rows, Open (the
// Dialogue window) and Start Entry's list of the file's entries; in Play its State, its Current
// Node (a click selects it in the Dialogue window) and Advance / the option buttons, to test by hand.

#include "window/InspectorWindow.h"

#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/DialogueRunnerComponent.h"
#include "dialogue/DialogueEditor.h"
#include "dialogue/DialogueRunner.h"
#include "dialogue/DialogueWindow.h"
#include "external/IconsFontAwesome6.h"
#include "graph/GraphEditorHooks.h"
#include "object/DialogueRunner.h"
#include "util/UIProbe.h"

#include "imgui.h"

using namespace jasperengine;

void editor::InspectorWindow::drawDialogueRunnerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    drawDefaultComponent(cpType, sceneProject, entities);
    if (entities.size() != 1) return;
    const Entity entity = entities[0];
    DialogueRunnerComponent* component = sceneProject->scene->findComponent<DialogueRunnerComponent>(entity);
    if (!component) return;
    const std::string path = component->dialogue.path;
    if (!path.empty()) {
        if (ImGui::SmallButton(ICON_FA_UP_RIGHT_FROM_SQUARE " Open")) {
            if (DialogueWindow* window = DialogueWindow::instance()) window->reveal(path, "");
        }
        UIProbe::item("inspector.dialoguerunner.open");
        ImGui::SameLine();
        const std::vector<std::string> entries = dialogues::entriesOf(project, path);
        ImGui::SetNextItemWidth(160.0f);
        const std::string shown = component->startEntry.empty() ? std::string("Start Entry: (default)") : "Start Entry: " + component->startEntry;
        if (ImGui::BeginCombo("##dialogue_entries", shown.c_str())) {
            std::vector<std::string> choices = {""};
            choices.insert(choices.end(), entries.begin(), entries.end());
            for (const std::string& choice : choices) {
                if (!ImGui::Selectable(choice.empty() ? "(default)" : choice.c_str(), choice == component->startEntry) || choice == component->startEntry) continue;
                CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "startEntry", choice));
            }
            ImGui::EndCombo();
        }
        UIProbe::item("inspector.dialoguerunner.entries");
    }
    if (sceneProject->playState == ScenePlayState::STOPPED) return;

    // the runner as it plays (runtime, never saved)
    DialogueRunner runner(sceneProject->scene, entity);
    ImGui::Separator();
    ImGui::Text("State: %s", component->state.c_str());
    if (!component->currentNode.empty()) {
        ImGui::TextUnformatted("Current Node:");
        ImGui::SameLine();
        if (ImGui::SmallButton(component->currentNode.c_str())) {
            if (DialogueWindow* window = DialogueWindow::instance()) window->reveal(path, component->currentNode);
        }
        UIProbe::item("inspector.dialoguerunner.current_node");
    }
    if (!runner.isRunning()) {
        if (!path.empty() && ImGui::Button(ICON_FA_PLAY " Start")) runner.start();
        UIProbe::item("inspector.dialoguerunner.start");
        return;
    }
    const dialogue::Line* line = runner.getCurrentLine();
    if (component->state == "line" && line) {
        if (!line->speakerName.empty()) ImGui::TextColored(ImVec4(0.6f, 0.75f, 1.0f, 1.0f), "%s", line->speakerName.c_str());
        ImGui::TextWrapped("%s", line->text.c_str());
        bool anyEnabled = false;
        for (const dialogue::Option& option : line->options) {
            anyEnabled = anyEnabled || option.enabled;
            ImGui::PushID(option.index);
            ImGui::BeginDisabled(!option.enabled);
            if (ImGui::Button((std::to_string(option.index) + ". " + option.text).c_str())) runner.choose(option.index);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!anyEnabled && ImGui::Button("Advance")) runner.advance();
        UIProbe::item("inspector.dialoguerunner.advance");
    } else if (component->state == "waiting") {
        if (ImGui::Button("Resume")) runner.resume();
    }
}
