// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationWindow.h"
#include "widget/ColorField.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavigationSettingsCmd.h"
#include "subsystem/NavigationSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

NavigationWindow::NavigationWindow(Project* project): project(project) {
}

void NavigationWindow::setOpen(bool open) {
    windowOpen = open;
}

void NavigationWindow::commit(const char* name) {
    draft.settings.sanitize();
    if (draft != project->getNavigationSettings())
        project->getProjectCommandHistory()->addCommand(new NavigationSettingsCmd(project, draft, name));
    dirty = false;
}

void NavigationWindow::show() {
    if (!windowOpen) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(640, 460)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    // what the project holds, unless an edit of this window is in progress
    if (!dirty) draft = project->getNavigationSettings();
    if (ImGui::BeginTabBar("##NavigationTabs")) {
        if (ImGui::BeginTabItem("Agents")) {
            drawAgents();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Areas")) {
            drawAreas();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Surfaces")) {
            drawSurfaces();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Grids")) {
            drawGrids();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void NavigationWindow::drawAgents() {
    std::vector<nav::AgentTypeSettings>& agents = draft.settings.agentTypes;
    int removeIndex = -1;
    if (ImGui::BeginTable("##NavAgents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Radius");
        ImGui::TableSetupColumn("Height");
        ImGui::TableSetupColumn("Step Height");
        ImGui::TableSetupColumn("Max Slope");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(28.0f));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < agents.size(); i++) {
            nav::AgentTypeSettings& agent = agents[i];
            ImGui::PushID((int)agent.id);
            ImGui::TableNextRow();
            auto field = [&](float& value, float minimum, float maximum, const char* format) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##v", &value, 0.01f, minimum, maximum, format)) dirty = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) commit("Change Agent Type");
                ImGui::PopID();
            };
            ImGui::TableNextColumn();
            char name[128];
            std::snprintf(name, sizeof(name), "%s", agent.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", name, sizeof(name))) {
                agent.name = name;
                dirty = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                // names stay unique and non-empty
                if (agent.name.empty()) agent.name = "Agent " + std::to_string(agent.id);
                for (size_t j = 0; j < agents.size(); j++) {
                    if (j != i && agents[j].name == agent.name) agent.name += " " + std::to_string(agent.id);
                }
                commit("Rename Agent Type");
            }
            ImGui::PushID("r");
            field(agent.radius, 0.01f, 100.0f, "%.2f m");
            ImGui::PushID("h");
            field(agent.height, 0.01f, 100.0f, "%.2f m");
            ImGui::PushID("s");
            field(agent.stepHeight, 0.0f, 100.0f, "%.2f m");
            ImGui::PushID("a");
            field(agent.maxSlope, 0.0f, 89.0f, "%.0f deg");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(agents.size() <= 1);
            if (ImGui::SmallButton(ICON_FA_TRASH_CAN)) removeIndex = (int)i;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(agents.size() <= 1 ? "A project keeps one agent type" : "Remove this agent type (surfaces and agents of it fall back to the first)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) {
        agents.erase(agents.begin() + removeIndex);
        commit("Remove Agent Type");
    }
    if (ImGui::Button(ICON_FA_PLUS " Add Agent Type")) {
        nav::AgentTypeSettings agent;
        agent.id = draft.settings.nextAgentTypeId();
        agent.name = "Agent " + std::to_string(agent.id);
        agents.push_back(agent);
        commit("Add Agent Type");
    }
    ImGui::TextDisabled("Surfaces bake for one agent type; a change makes their bakes stale.");
}

void NavigationWindow::drawAreas() {
    if (!ImGui::BeginTable("##NavAreas", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(28.0f));
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Cost");
    ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(60.0f));
    ImGui::TableHeadersRow();
    for (int i = 0; i < nav::AREA_COUNT; i++) {
        nav::AreaSettings& area = draft.settings.areas[(size_t)i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", i);
        ImGui::TableNextColumn();
        const bool fixed = i <= nav::AREA_JUMP;
        ImGui::BeginDisabled(fixed);
        char name[128];
        std::snprintf(name, sizeof(name), "%s", area.name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", name, sizeof(name))) {
            area.name = name;
            dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit("Rename Area");
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(i == nav::AREA_NOT_WALKABLE);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##cost", &area.cost, 0.05f, 1.0f, 1000.0f, "%.2f")) {
            area.cost = std::max(1.0f, area.cost);
            dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) commit("Change Area Cost");
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ui::ColorFieldOptions colorOptions;
        colorOptions.linearStorage = false;
        colorOptions.label = "Area Colour";
        Vector4 color = area.color;
        const ui::EditPhase colorPhase = ui::colorField("##color", color, colorOptions);
        if (colorPhase == ui::EditPhase::Changing || colorPhase == ui::EditPhase::Cancelled) {
            area.color = color;
            dirty = true;
        }
        // the window's session is one Navigation settings step (a cancel restores, adding none)
        if (colorPhase == ui::EditPhase::Committed || colorPhase == ui::EditPhase::Cancelled) commit("Change Area Colour");
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Costs are per metre (>= 1) and apply at once; Not Walkable is never entered.");
}

void NavigationWindow::drawSurfaces() {
    NavigationEditor& navEditor = NavigationEditor::get();
    bool any = false;
    for (SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;
        auto array = sceneProject.scene->getComponentArray<NavMeshSurfaceComponent>();
        if (array->size() == 0) continue;
        any = true;
        NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
        ImGui::PushID((int)sceneProject.id);
        ImGui::SeparatorText(sceneProject.name.c_str());
        const bool playing = sceneProject.playState != ScenePlayState::STOPPED;
        ImGui::BeginDisabled(playing || !nav::isNavMeshAvailable());
        if (ImGui::Button(ICON_FA_HAMMER " Bake All")) {
            std::string error;
            if (navEditor.bakeAll(project, sceneProject.id, NavBakeMode::DIRTY, error) == 0 && !error.empty()) Out::error("Bake All: %s", error.c_str());
        }
        ImGui::EndDisabled();
        if (ImGui::BeginTable("##NavSurfaces", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Surface");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Tiles / Polys");
            ImGui::TableSetupColumn("Memory");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
            ImGui::TableHeadersRow();
            std::vector<Entity> entities;
            for (size_t i = 0; i < array->size(); i++) entities.push_back(array->getEntity(i));
            std::sort(entities.begin(), entities.end());
            for (Entity entity : entities) {
                ImGui::PushID((int)entity);
                const NavSurfaceStatus status = system ? system->getSurfaceStatus(entity) : NavSurfaceStatus();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sceneProject.scene->getEntityName(entity).c_str());
                ImGui::TableNextColumn();
                const bool baking = status.state == NavBakeState::BAKING || navEditor.isBaking(sceneProject.id, entity);
                if (baking) {
                    ImGui::ProgressBar(status.progress, ImVec2(-1, 0));
                } else if (status.state == NavBakeState::NOT_BAKED) {
                    ImGui::TextUnformatted(status.loadError.empty() ? "Not baked" : "Not baked (unreadable asset)");
                } else {
                    const int dirtyTiles = playing ? status.dirtyTiles : navEditor.dirtyTiles(project, sceneProject.id, entity);
                    if (dirtyTiles > 0) ImGui::TextColored(Theme::Colors::WarningText, "Dirty %d tiles", dirtyTiles);
                    else ImGui::TextUnformatted("Baked");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", status.tiles, status.polys);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f KB", (double)status.bytes / 1024.0);
                ImGui::TableNextColumn();
                std::string error;
                if (baking) {
                    if (ImGui::SmallButton("Cancel")) navEditor.cancel(project, sceneProject.id, entity);
                } else {
                    ImGui::BeginDisabled(playing || !nav::isNavMeshAvailable());
                    if (ImGui::SmallButton("Bake") && !navEditor.bake(project, sceneProject.id, entity, NavBakeMode::DIRTY, error))
                        Out::error("Bake: %s", error.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear") && !navEditor.clear(project, sceneProject.id, entity, error))
                        Out::error("Clear: %s", error.c_str());
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Select")) project->setSelectedEntity(sceneProject.id, entity);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("No NavMesh Surface in the open scenes: add one (Add Component > Navigation) to the ground.");
}

void NavigationWindow::drawGrids() {
    bool any = false;
    for (SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;
        auto array = sceneProject.scene->getComponentArray<NavGridComponent>();
        if (array->size() == 0) continue;
        any = true;
        ImGui::PushID((int)sceneProject.id);
        ImGui::SeparatorText(sceneProject.name.c_str());
        std::vector<Entity> entities;
        for (size_t i = 0; i < array->size(); i++) entities.push_back(array->getEntity(i));
        std::sort(entities.begin(), entities.end());
        for (Entity entity : entities) {
            ImGui::PushID((int)entity);
            const NavGridComponent& grid = sceneProject.scene->getComponent<NavGridComponent>(entity);
            ImGui::Text("%s: %d x %d %s", sceneProject.scene->getEntityName(entity).c_str(), grid.width, grid.height,
                        grid.layout == nav::GridLayout::Hex ? "hex" : "square");
            ImGui::SameLine();
            ImGui::BeginDisabled(grid.source == NavGridSource::NONE || sceneProject.playState != ScenePlayState::STOPPED);
            if (ImGui::SmallButton("Rebuild from source")) NavigationEditor::get().rebuildGrid(project, sceneProject.id, entity);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Select")) project->setSelectedEntity(sceneProject.id, entity);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("No Nav Grid in the open scenes.");
}
