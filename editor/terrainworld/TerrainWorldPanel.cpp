// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldPanel.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/PropertyCmd.h"
#include "command/type/TerrainWorldTilesCmd.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terrain;

namespace {

    ImU32 tileStateColour(TileState state, bool pinned) {
        if (pinned) return IM_COL32(230, 180, 60, 255);
        switch (state) {
            case TileState::Absent: return IM_COL32(40, 40, 40, 255);
            case TileState::Cold: return IM_COL32(70, 90, 120, 255);
            case TileState::Queued: case TileState::Loading: return IM_COL32(120, 120, 200, 255);
            case TileState::CpuReady: case TileState::Uploading: return IM_COL32(90, 160, 200, 255);
            case TileState::Resident: return IM_COL32(80, 190, 110, 255);
            case TileState::Error: return IM_COL32(210, 70, 70, 255);
        }
        return IM_COL32(40, 40, 40, 255);
    }

    std::string worldMegabytes(uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / 1048576.0);
        return text;
    }

}

// ------------------------------------------------------------------ Create Terrain World

CreateTerrainWorldDialog& CreateTerrainWorldDialog::instance() {
    static CreateTerrainWorldDialog dialog;
    return dialog;
}

void CreateTerrainWorldDialog::open(Project* project, uint32_t sceneId, Entity parent) {
    this->project = project;
    this->sceneId = sceneId;
    this->parent = parent;
    settings = TerrainWorldSettings();
    error.clear();
    isOpen = true;
    justOpened = true;
}

void CreateTerrainWorldDialog::show() {
    if (!isOpen || !project) return;
    const char* popupName = "Create Terrain World##CreateTerrainWorldModal";
    if (justOpened) {
        ImGui::OpenPopup(popupName);
        justOpened = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin(popupName, &isOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    EditorModal::message("A grid of flat tiles that streams around the camera: its folder asset, the GameObject and its Rigidbody, one undo step.");
    EditorModal::error(error);

    const float width = ImGui::GetFontSize() * 12.0f;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", settings.name.c_str());
    ImGui::SetNextItemWidth(width);
    if (EditorModal::inputText("Name", "name", name, sizeof(name))) settings.name = name;
    char path[256];
    std::snprintf(path, sizeof(path), "%s", settings.assetPath.c_str());
    ImGui::SetNextItemWidth(width);
    if (EditorModal::inputText("Asset Path", "asset_path", path, sizeof(path))) settings.assetPath = path;
    int quadsIndex = 0;
    const int quads[] = {64, 128, 256, 512, 1024};
    for (int i = 0; i < 5; i++) if (quads[i] == settings.tileQuads) quadsIndex = i;
    ImGui::SetNextItemWidth(width);
    if (EditorModal::combo("Tile Size", "tile_quads", &quadsIndex, {"64 quads", "128 quads", "256 quads", "512 quads", "1024 quads"})) {
        settings.tileQuads = quads[quadsIndex];
    }
    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Spacing", "spacing", &settings.spacing, 0.0f, 0.0f, "%.2f m");
    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Max Height", "max_height", &settings.maxHeight, 0.0f, 0.0f, "%.1f m");
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Grid Width", "grid_width", &settings.gridWidth, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Grid Length", "grid_length", &settings.gridLength, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Base Height", "base_height", &settings.baseHeight, 0.0f, 0.0f, "%.1f m");
    settings.gridWidth = std::clamp(settings.gridWidth, 1, WORLD_MAX_BOX_DESKTOP);
    settings.gridLength = std::clamp(settings.gridLength, 1, WORLD_MAX_BOX_DESKTOP);
    settings.originX = -settings.gridWidth / 2;
    settings.originZ = -settings.gridLength / 2;
    const float side = static_cast<float>(settings.tileQuads) * settings.spacing;
    ImGui::TextDisabled("%.0f m x %.0f m, %i tiles; about %s on the GPU at once", side * settings.gridWidth, side * settings.gridLength,
                        settings.gridWidth * settings.gridLength, worldMegabytes(predictTerrainWorldBytes(settings)).c_str());
    // the 16-bit range steps max_height / 65535: over 2,000 m that is past 3 cm (spec risks)
    if (settings.maxHeight > 2000.0f) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Heights step %.1f cm at this Max Height", settings.maxHeight / 655.35f);

    ImGui::Separator();
    if (EditorModal::button("Create", "ok", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        if (createTerrainWorld(project, sceneId, parent, settings, error) != NULL_ENTITY) {
            isOpen = false;
            ImGui::CloseCurrentPopup();
            TerrainWorldPanel::instance().open(project);
        }
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ------------------------------------------------------------------ the panel

TerrainWorldPanel& TerrainWorldPanel::instance() {
    static TerrainWorldPanel panel;
    return panel;
}

void TerrainWorldPanel::open(Project* project) {
    this->project = project;
    isOpen = true;
    ImGui::SetWindowFocus(WINDOW_NAME);
}

const char* TerrainWorldPanel::stateHelp(int state) {
    switch (static_cast<TileState>(state)) {
        case TileState::Absent: return "Absent: no tile here. Nothing draws and nothing collides.";
        case TileState::Cold: return "Cold: on disk, not in memory. Draws from the overview.";
        case TileState::Queued: return "Queued: waiting for a read.";
        case TileState::Loading: return "Loading: being read and decoded.";
        case TileState::CpuReady: return "In memory: collision and queries are exact; it draws from the overview until its page is up.";
        case TileState::Uploading: return "Uploading: its page goes to the GPU this frame.";
        case TileState::Resident: return "Resident: drawn from its own page.";
        case TileState::Error: return "Error: the file could not be read. Draws from the overview; a changed file clears it.";
    }
    return "";
}

void TerrainWorldPanel::show() {
    if (!isOpen || !project) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &isOpen)) {
        ImGui::End();
        return;
    }
    SceneProject* sceneProject = project->getSelectedScene();
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    const std::vector<Entity> selected = sceneProject ? project->getSelectedEntities(sceneProject->id) : std::vector<Entity>();
    const Entity entity = selected.size() == 1 ? selected[0] : NULL_ENTITY;
    TerrainWorldComponent* component = scene && entity != NULL_ENTITY ? scene->findComponent<TerrainWorldComponent>(entity) : nullptr;
    std::shared_ptr<TerrainWorldSystem> system = scene ? scene->getSystem<TerrainWorldSystem>() : nullptr;
    std::string error;
    TerrainWorldRuntime* runtime = component && system ? system->ensureRuntime(entity, &error) : nullptr;
    if (!component) {
        ImGui::TextWrapped("Select a Terrain World, or make one.");
        if (sceneProject && ImGui::Button(ICON_FA_MAP " Create Terrain World...")) CreateTerrainWorldDialog::instance().open(project, sceneProject->id, NULL_ENTITY);
        ImGui::End();
        return;
    }
    if (!runtime) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error.c_str());
        ImGui::End();
        return;
    }

    // the tool of a click on a cell
    int toolIndex = static_cast<int>(tool);
    ImGui::RadioButton("Select", &toolIndex, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Add Tile", &toolIndex, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Remove Tile", &toolIndex, 2);
    tool = static_cast<Tool>(toolIndex);

    // R41: the minimap, the box of present tiles and a ring of one around it
    const WorldLattice& lattice = runtime->lattice();
    TileCoord low(-2, -2), high(1, 1);
    if (runtime->occupancy().bounds(low, high)) {
        low = TileCoord(low.x - 1, low.z - 1);
        high = TileCoord(high.x + 1, high.z + 1);
    }
    const int columns = high.x - low.x + 1, rows = high.z - low.z + 1;
    const float available = ImGui::GetContentRegionAvail().x;
    const float cell = std::clamp(available / static_cast<float>(std::max(columns, rows)), 3.0f, 24.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("minimap", ImVec2(cell * columns, cell * rows));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    TileCoord hoveredTile;
    bool hasHover = false;
    for (int z = low.z; z <= high.z; z++) {
        for (int x = low.x; x <= high.x; x++) {
            const TileCoord tile(x, z);
            // +z up the minimap, as the Scene view seen from above
            const ImVec2 a(origin.x + (x - low.x) * cell, origin.y + (high.z - z) * cell);
            const ImVec2 b(a.x + cell - 1.0f, a.y + cell - 1.0f);
            draw->AddRectFilled(a, b, tileStateColour(runtime->tileState(tile), runtime->tiles().pinned(tile)));
            if (hovered && mouse.x >= a.x && mouse.x < b.x + 1.0f && mouse.y >= a.y && mouse.y < b.y + 1.0f) {
                hoveredTile = tile;
                hasHover = true;
                draw->AddRect(a, b, IM_COL32(255, 255, 255, 255));
            }
        }
    }
    // the focuses
    for (const StreamFocus& focus : runtime->tiles().focuses()) {
        const float fx = (focus.x / lattice.tileSize() - low.x) * cell, fz = (high.z + 1 - focus.z / lattice.tileSize()) * cell;
        draw->AddCircleFilled(ImVec2(origin.x + fx, origin.y + fz), std::max(2.5f, cell * 0.2f), IM_COL32(255, 255, 255, 230));
    }
    if (hasHover) {
        ImGui::SetTooltip("Tile (%i, %i)\n%s", hoveredTile.x, hoveredTile.z, stateHelp(static_cast<int>(runtime->tileState(hoveredTile))));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && tool != Tool::Select && !runtime->playing()) {
            std::string why;
            if (!editTerrainWorldTiles(project, sceneProject->id, entity, {hoveredTile}, tool == Tool::RemoveTile, why)) message = why;
            else message.clear();
        }
    }
    if (!message.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", message.c_str());

    if (!runtime->playing()) {
        if (ImGui::Button("Rebuild Overview")) runtime->rebuildOverview(false);
        ImGui::SameLine();
        if (ImGui::Button("Repair Seams")) {
            int repaired = 0;
            for (const TileCoord& tile : runtime->occupancy().presentTiles()) repaired += runtime->checkSeams(tile, true);
            message = std::to_string(repaired) + " border samples repaired";
        }
    }

    // R41's settings, through the component so they undo
    ImGui::SeparatorText("Streaming");
    auto setInt = [&](const char* label, const char* property, int value, int low, int highest) {
        int edited = value;
        if (ImGui::InputInt(label, &edited) && edited != value) {
            CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<int>(project, sceneProject->id, entity, ComponentType::TerrainWorldComponent,
                                                                                  property, std::clamp(edited, low, highest)));
        }
    };
    auto setFloat = [&](const char* label, const char* property, float value) {
        float edited = value;
        if (ImGui::InputFloat(label, &edited) && edited != value) {
            CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<float>(project, sceneProject->id, entity, ComponentType::TerrainWorldComponent,
                                                                                    property, std::max(edited, 0.0f)));
        }
    };
    setInt("Pool Size", "poolSize", component->poolSize, 0, 512);
    setFloat("LOD Distance", "lodDistance", component->lodDistance);
    setFloat("Upload Budget (MiB)", "uploadBudget", component->uploadBudget);
    setFloat("Collision Radius", "collisionRadius", component->collisionRadius);
    bool autoAdd = component->autoAddTiles;
    if (ImGui::Checkbox("Auto Add Tiles", &autoAdd)) {
        CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<bool>(project, sceneProject->id, entity, ComponentType::TerrainWorldComponent,
                                                                               "autoAddTiles", autoAdd));
    }

    // R41's live info
    ImGui::SeparatorText("Info");
    const TerrainWorldStats stats = runtime->stats();
    ImGui::Text("Tiles: %i, resident %i, in memory %i, errors %i", stats.tiles, stats.states[static_cast<int>(TileState::Resident)],
                stats.states[static_cast<int>(TileState::CpuReady)] + stats.states[static_cast<int>(TileState::Resident)], stats.states[static_cast<int>(TileState::Error)]);
    ImGui::Text("GPU %s of %s, CPU %s of %s", worldMegabytes(stats.gpuBytes).c_str(), worldMegabytes(stats.gpuCap).c_str(), worldMegabytes(stats.cpuBytes).c_str(),
                worldMegabytes(stats.cpuCap).c_str());
    ImGui::Text("Queue %i, loading %i, uploads %s this frame", stats.queue, stats.inFlight, worldMegabytes(stats.uploadBytesLastFrame).c_str());
    ImGui::Text("Draws %i colour, %i depth; nodes %i visited, %i drawn", stats.draws[0][0], stats.draws[0][1], stats.nodesVisited, stats.nodesSelected);
    ImGui::Text("Collision: %i fine, %i coarse, %i frozen", stats.collisionFine, stats.collisionCoarse, stats.frozenBodies);
    ImGui::Text("Main thread %.2f ms (worst %.2f)", stats.mainThreadMs, stats.worstMainThreadMs);
    if (stats.seamErrors > 0) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%i seam errors: Repair Seams resolves them", stats.seamErrors);
    for (const std::string& line : stats.errors) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", line.c_str());
    ImGui::End();
}

void TerrainWorldPanel::drawInspectorNotes(Project* project, SceneProject* sceneProject) {
    if (!sceneProject || !sceneProject->scene) return;
    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() == 1) {
        if (std::shared_ptr<TerrainWorldSystem> system = sceneProject->scene->getSystem<TerrainWorldSystem>()) {
            const std::string error = system->getError(selected[0]);
            if (!error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error.c_str());
        }
    }
    // T3's Macro Normal and Base Map macro are one texture over one terrain (spec Scope)
    ImGui::TextDisabled("Macro Normal and the Base Map macro are not available on a Terrain World.");
    if (ImGui::Button(ICON_FA_MAP " Open Terrain World Panel", ImVec2(-1, 0))) instance().open(project);
}
