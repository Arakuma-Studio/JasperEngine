// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayModeChangesWindow.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"

#include "imgui.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // What the row changes, as the Property column shows it
    std::string rowLabel(const KeepRow& row) {
        switch (row.kind) {
            case KeepRow::Kind::Property: return CommandNames::property(row.property);
            case KeepRow::Kind::Transform: return "Position, Rotation, Scale";
            case KeepRow::Kind::Component: return row.property.empty() ? std::string("Values") : row.property;
            case KeepRow::Kind::AddComponent: return "Added";
            case KeepRow::Kind::RemoveComponent: return "Removed";
            case KeepRow::Kind::Name: return "Name";
            case KeepRow::Kind::State: return row.property;
            case KeepRow::Kind::Created: return "Created";
            case KeepRow::Kind::Deleted: return "Deleted";
            case KeepRow::Kind::Unsupported: return row.property;
        }
        return row.property;
    }

    std::string componentLabel(const KeepRow& row) {
        return row.hasComponent ? CommandNames::component(row.component) : std::string("GameObject");
    }

}

PlayModeChangesWindow::PlayModeChangesWindow(Project* project): project(project) {
}

void PlayModeChangesWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    if (!open && windowOpen) {
        // closing the window discards (R35)
        discard();
    }
    windowOpen = open;
}

void PlayModeChangesWindow::openFor(uint32_t id) {
    sceneId = id;
    checked.clear();
    if (const PlayChangeReview::Pending* pending = PlayChangeReview::get().pending(id)) {
        for (const KeepRow& row : pending->rows) {
            checked[row.id] = row.keepable;
        }
    }
    focusRequested = true;
    windowOpen = true;
}

void PlayModeChangesWindow::keep(bool all) {
    std::vector<uint64_t> ids;
    for (const auto& [id, keepIt] : checked) {
        if (all || keepIt) ids.push_back(id);
    }
    const PlayChangeReview::ApplyResult result = PlayChangeReview::get().apply(project, sceneId, &ids);
    for (const auto& [rowId, why] : result.skipped) {
        Out::warning("Play Mode Changes: a change was not kept: %s", why.c_str());
    }
    checked.clear();
}

void PlayModeChangesWindow::discard() {
    PlayChangeReview::get().discard(sceneId);
    checked.clear();
}

void PlayModeChangesWindow::show() {
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    bool open = true;
    if (!ImGui::Begin(WINDOW_NAME, &open)) {
        ImGui::End();
        if (!open) setOpen(false);
        return;
    }

    // the scene the review is for, else any scene with one
    const PlayChangeReview::Pending* pending = PlayChangeReview::get().pending(sceneId);
    if (!pending) {
        for (uint32_t id : PlayChangeReview::get().pendingScenes()) {
            openFor(id);
            pending = PlayChangeReview::get().pending(id);
            break;
        }
    }
    if (!pending) {
        ImGui::TextDisabled("No changes from Play mode to review.");
        ImGui::TextDisabled("Edits made while playing are listed here after Stop.");
        ImGui::End();
        if (!open) setOpen(false);
        return;
    }

    ImGui::Text("Changes made while '%s' played. Keep the ones you want: they are applied as one undo step.", pending->sceneName.c_str());

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##PlayModeChanges", 4, flags, ImVec2(0, -footerHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Edit Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Play Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();

        // GameObject > component > rows, in the order the rows came
        std::vector<Entity> gameObjects;
        for (const KeepRow& row : pending->rows) {
            if (std::find(gameObjects.begin(), gameObjects.end(), row.entity) == gameObjects.end()) gameObjects.push_back(row.entity);
        }
        for (Entity entity : gameObjects) {
            std::string name;
            for (const KeepRow& row : pending->rows) {
                if (row.entity == entity) { name = row.entity == NULL_ENTITY ? "Other Steps" : row.gameObject; break; }
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)entity);
            const bool expanded = ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns);
            // a click on the GameObject selects it (an undo step like any selection)
            if (ImGui::IsItemClicked() && entity != NULL_ENTITY && project->getScene(sceneId)) {
                SceneProject* scene = project->getScene(sceneId);
                if (scene->scene->isEntityCreated(entity)) project->setSelectedEntity(sceneId, entity);
            }
            if (expanded) {
                std::vector<std::string> components;
                for (const KeepRow& row : pending->rows) {
                    if (row.entity != entity) continue;
                    const std::string component = componentLabel(row);
                    if (std::find(components.begin(), components.end(), component) == components.end()) components.push_back(component);
                }
                for (const std::string& component : components) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool open = ImGui::TreeNodeEx(component.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns);
                    if (!open) continue;
                    for (const KeepRow& row : pending->rows) {
                        if (row.entity != entity || componentLabel(row) != component) continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID((int)row.id);
                        ImGui::BeginDisabled(!row.keepable);
                        bool keepIt = checked[row.id] && row.keepable;
                        if (ImGui::Checkbox(rowLabel(row).c_str(), &keepIt)) checked[row.id] = keepIt;
                        ImGui::EndDisabled();
                        if (!row.keepable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("%s", row.reason.c_str());
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.editValue.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(row.playValue.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", row.source.c_str());
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Keep Selected")) keep(false);
    ImGui::SameLine();
    if (ImGui::Button("Keep All")) keep(true);
    ImGui::SameLine();
    if (ImGui::Button("Discard")) discard();

    ImGui::End();
    if (!open) setOpen(false);
}
