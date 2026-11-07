// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationWindow.h"
#include "widget/ColorField.h"

#include "App.h"
#include "Backend.h"
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
#include "window/SceneWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

void NavigationWindow::refresh() {
    const double now = ImGui::GetTime();
    if (refreshedAt >= 0.0 && now - refreshedAt < REFRESH_SECONDS) return;
    refreshedAt = now;
    NavigationEditor& navEditor = NavigationEditor::get();
    ProjectFindingOptions options;
    // the stale counts the editor already keeps (one collection a second at most)
    options.dirtyTilesOf = [this, &navEditor](uint32_t sceneId, Entity surface) { return navEditor.dirtyTiles(project, sceneId, surface); };
    findings = collectProjectFindings(project, nullptr, options);
    coverages.clear();
    for (SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;
        NavigationSystem* system = NavigationEditor::systemOf(sceneProject);
        auto array = sceneProject.scene->getComponentArray<NavMeshSurfaceComponent>();
        for (size_t i = 0; system && i < array->size(); i++) {
            Coverage coverage;
            coverage.sceneId = sceneProject.id;
            coverage.surface = array->getEntity(i);
            coverage.coverage = system->getCoverage(coverage.surface);
            coverages.push_back(coverage);
        }
    }
}

void NavigationWindow::drawSurfaces() {
    refresh();
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
            // (NavigationEditor logs what it refuses)
            std::string error;
            navEditor.bakeAll(project, sceneProject.id, NavBakeMode::DIRTY, error);
        }
        ImGui::EndDisabled();
        if (ImGui::BeginTable("##NavSurfaces", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Surface");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Last Bake", ImGuiTableColumnFlags_None, 1.6f);
            ImGui::TableSetupColumn("Tiles / Polys");
            ImGui::TableSetupColumn("Walkable");
            ImGui::TableSetupColumn("Islands", ImGuiTableColumnFlags_None, 0.6f);
            ImGui::TableSetupColumn("Memory");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
            ImGui::TableHeadersRow();
            std::vector<Entity> entities;
            for (size_t i = 0; i < array->size(); i++) entities.push_back(array->getEntity(i));
            std::sort(entities.begin(), entities.end());
            for (Entity entity : entities) {
                ImGui::PushID((int)entity);
                const NavSurfaceStatus status = system ? system->getSurfaceStatus(entity) : NavSurfaceStatus();
                const std::string id = std::to_string(sceneProject.id) + "." + std::to_string(entity);
                // a cell's click selects the surface, a double-click frames it too
                auto cell = [&](const std::string& text, const char* key) {
                    if (ImGui::Selectable((text + "##" + key + id).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        project->setSelectedEntity(sceneProject.id, entity);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            if (SceneWindow* sceneWindow = Backend::getApp().getSceneWindow()) sceneWindow->frameEntities(sceneProject.id, sceneProject.id, {entity});
                        }
                    }
                };
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                cell(sceneProject.scene->getEntityName(entity), "name");
                ImGui::TableNextColumn();
                const bool baking = status.state == NavBakeState::BAKING || navEditor.isBaking(sceneProject.id, entity);
                const NavigationEditor::LastBake* failed = navEditor.lastBake(sceneProject.id, entity);
                if (baking) {
                    ImGui::ProgressBar(status.progress, ImVec2(-1, 0));
                } else if (failed && !failed->ok && !failed->cancelled) {
                    // the session's last bake failed (S74 R3): until a bake succeeds
                    ImGui::TextColored(Theme::Colors::ErrorText, "Bake failed");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", failed->error.c_str());
                } else if (status.state == NavBakeState::NOT_BAKED) {
                    ImGui::TextUnformatted(status.loadError.empty() ? "Not baked" : "Not baked (unreadable asset)");
                } else {
                    const int dirtyTiles = playing ? status.dirtyTiles : navEditor.dirtyTiles(project, sceneProject.id, entity);
                    if (dirtyTiles > 0) ImGui::TextColored(Theme::Colors::WarningText, "Dirty %d tiles", dirtyTiles);
                    else ImGui::TextUnformatted("Baked");
                }
                // the last bake of this session: time, tiles, warnings; the row opens to them, the
                // error and its hint
                ImGui::TableNextColumn();
                const NavBakeResult* last = system ? system->getLastBakeResult(entity) : nullptr;
                if (!last) {
                    ImGui::TextDisabled("\xE2\x80\x93");
                    ImGui::SetItemTooltip("Not baked in this session");
                } else {
                    char summary[160];
                    if (last->ok) {
                        std::snprintf(summary, sizeof(summary), "%.0f ms, %d/%d tiles", last->milliseconds, last->tilesBuilt, last->tilesTotal);
                    } else {
                        std::snprintf(summary, sizeof(summary), "%s", last->cancelled ? "Cancelled" : "Failed");
                    }
                    std::string label = summary;
                    if (!last->warnings.empty()) label += "  " ICON_FA_TRIANGLE_EXCLAMATION " " + std::to_string(last->warnings.size());
                    const bool details = !last->warnings.empty() || !last->ok;
                    if (!last->ok && !last->cancelled) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
                    const bool open = ImGui::TreeNodeEx((label + "##lastbake" + id).c_str(),
                                                        ImGuiTreeNodeFlags_SpanAvailWidth | (details ? 0 : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));
                    if (!last->ok && !last->cancelled) ImGui::PopStyleColor();
                    if (open && details) {
                        ImGui::PushTextWrapPos(0.0f);
                        if (!last->ok && !last->error.empty()) {
                            ImGui::TextColored(Theme::Colors::ErrorText, "%s (%s)", last->error.c_str(), nav::bakeErrorName(last->errorCode));
                            if (!last->hint.empty()) ImGui::TextDisabled("%s", last->hint.c_str());
                        }
                        for (const std::string& warning : last->warnings) ImGui::TextColored(Theme::Colors::WarningText, "%s", warning.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::TreePop();
                    }
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", status.tiles, status.polys);
                const auto coverage = std::find_if(coverages.begin(), coverages.end(), [&](const Coverage& c) {
                    return c.sceneId == sceneProject.id && c.surface == entity;
                });
                char text[64];
                ImGui::TableNextColumn();
                if (coverage != coverages.end() && status.state != NavBakeState::NOT_BAKED) {
                    std::snprintf(text, sizeof(text), "%.1f m\xC2\xB2", coverage->coverage.walkableArea);
                    cell(text, "walkable");
                    ImGui::SetItemTooltip("Walkable area; %.0f%% of its tiles' footprint", coverage->coverage.fraction * 100.0f);
                    ImGui::TableNextColumn();
                    std::snprintf(text, sizeof(text), "%d", coverage->coverage.islandCount);
                    cell(text, "islands");
                    ImGui::SetItemTooltip("Connected parts (links join them); largest %.1f m\xC2\xB2. Scene view: Color By Islands",
                                          coverage->coverage.largestIslandArea);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x93");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("\xE2\x80\x93");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.1f KB", (double)status.bytes / 1024.0);
                ImGui::TableNextColumn();
                std::string error;
                if (baking) {
                    if (ImGui::SmallButton("Cancel")) navEditor.cancel(project, sceneProject.id, entity);
                } else {
                    ImGui::BeginDisabled(playing || !nav::isNavMeshAvailable());
                    if (ImGui::SmallButton("Bake")) navEditor.bake(project, sceneProject.id, entity, NavBakeMode::DIRTY, error);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) navEditor.clear(project, sceneProject.id, entity, error);
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
    drawProblems();
}

void NavigationWindow::drawProblems() {
    ImGui::Spacing();
    const std::string header = "Problems (" + std::to_string(findings.size()) + ")###NavProblems";
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (findings.empty()) {
        ImGui::TextDisabled("No navigation problems in the open scenes.");
        return;
    }
    int index = 0;
    for (const ProjectNavFinding& item : findings) {
        ImGui::PushID(index++);
        const nav::FindingSeverity severity = item.finding.severity;
        const char* icon = severity == nav::FindingSeverity::Error ? ICON_FA_CIRCLE_XMARK
                         : severity == nav::FindingSeverity::Warning ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO;
        const ImVec4 color = severity == nav::FindingSeverity::Error ? Theme::Colors::ErrorText
                           : severity == nav::FindingSeverity::Warning ? Theme::Colors::WarningText : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(color, "%s", icon);
        ImGui::SameLine();
        const std::string who = item.entityName.empty() ? item.sceneName : item.entityName;
        // the id names the finding (find_ui_item "##navproblem.<code>.<entity>")
        const std::string label = item.finding.code + "  " + who + ": " + item.finding.message + "##navproblem." + item.finding.code + "." +
                                  std::to_string(item.finding.entity);
        // a click selects its GameObject, a double-click frames it in the Scene view
        const bool selectable = item.finding.entity != NULL_ENTITY && item.sceneId != 0;
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && selectable) {
            project->setSelectedEntity(item.sceneId, item.finding.entity);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (SceneWindow* sceneWindow = Backend::getApp().getSceneWindow())
                    sceneWindow->frameEntities(item.sceneId, item.sceneId, {item.finding.entity});
            }
        }
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::Text("%s (%s) in %s", item.finding.code.c_str(), nav::findingSeverityName(severity), item.scenePath.empty() ? item.sceneName.c_str() : item.scenePath.c_str());
            ImGui::TextUnformatted(item.finding.message.c_str());
            for (const auto& detail : item.finding.details) ImGui::TextDisabled("%s: %g", detail.first.c_str(), detail.second);
            if (selectable) ImGui::TextDisabled("Click: select; double-click: frame it");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
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
