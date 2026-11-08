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
#include "terrainworld/TerrainWorldConvert.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "terrainworld/TerrainWorldImport.h"
#include "terrainworld/TerrainWorldOverlay.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terrain;

namespace {

    ImU32 tileStateColour(TileState state, bool pinned) {
        // the overlay's Tile State colours (R41, R42)
        const Vector4 colour = terrainWorldStateColour(state, pinned);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x, colour.y, colour.z, colour.w));
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

// ------------------------------------------------------------------ convert

ConvertTerrainWorldDialog& ConvertTerrainWorldDialog::instance() {
    static ConvertTerrainWorldDialog dialog;
    return dialog;
}

void ConvertTerrainWorldDialog::open(Project* project, uint32_t sceneId, Entity source) {
    if (running) return;
    this->project = project;
    this->sceneId = sceneId;
    this->source = source;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    name = sceneProject && sceneProject->scene ? sceneProject->scene->getEntityName(source) + " World" : "Terrain World";
    error.clear();
    isOpen = true;
    justOpened = true;
}

void ConvertTerrainWorldDialog::show() {
    if (!isOpen || !project) return;
    const char* popupName = "Convert to Terrain World##ConvertTerrainWorldModal";
    if (justOpened) {
        ImGui::OpenPopup(popupName);
        justOpened = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool keepOpen = true;
    if (!EditorModal::begin(popupName, running ? nullptr : &keepOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (!running) isOpen = keepOpen && isOpen;
        return;
    }
    EditorModal::message("A new Terrain World at the Terrain's place, from its heights, paint, holes, details and trees, one undo step. "
                         "The Terrain stays, inactive.");
    EditorModal::error(error);

    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    TerrainConvertOptions options;
    options.name = name;
    TerrainConvertPlan plan;
    std::string why;
    const bool possible = planTerrainConversion(scene, source, options, plan, why);
    const float width = ImGui::GetFontSize() * 12.0f;
    char text[128];
    std::snprintf(text, sizeof(text), "%s", name.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(running);
    if (EditorModal::inputText("Name", "name", text, sizeof(text))) name = text;
    ImGui::EndDisabled();
    if (possible) {
        const float side = static_cast<float>(plan.world.tileQuads) * plan.world.spacing;
        ImGui::TextDisabled("%i x %i tiles of %.0f m (%i quads at %.2f m)", plan.world.gridWidth, plan.world.gridLength, side, plan.world.tileQuads,
                            plan.world.spacing);
    } else {
        EditorModal::error(why);
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!possible || running);
    if (EditorModal::button(running ? "Converting..." : "Convert", "ok", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        running = true;
        error.clear();
        cancel = convertTerrainLater(project, sceneId, source, options, [this](const TerrainConvertResult& result) {
            running = false;
            cancel = nullptr;
            if (result.entity == NULL_ENTITY) {
                error = result.error;
                return;
            }
            isOpen = false;
            TerrainWorldPanel::instance().open(project);
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        if (running && cancel) cancel();
        if (!running) {
            isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    if (!isOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ------------------------------------------------------------------ Import Heightmap

ImportHeightmapDialog& ImportHeightmapDialog::instance() {
    static ImportHeightmapDialog dialog;
    return dialog;
}

void ImportHeightmapDialog::open(Project* project, uint32_t sceneId, Entity world) {
    if (task && !task->finished()) return;
    this->project = project;
    this->sceneId = sceneId;
    this->world = world;
    error.clear();
    done.clear();
    task.reset();
    isOpen = true;
    justOpened = true;
}

void ImportHeightmapDialog::show() {
    if (!isOpen || !project) return;
    const char* popupName = "Import Heightmap##TerrainWorldImportModal";
    if (justOpened) {
        ImGui::OpenPopup(popupName);
        justOpened = false;
    }
    const bool running = task && !task->finished();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool keepOpen = true;
    if (!EditorModal::begin(popupName, running ? nullptr : &keepOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (!running) isOpen = keepOpen && isOpen;
        return;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    std::shared_ptr<TerrainWorldSystem> system = sceneProject && sceneProject->scene ? sceneProject->scene->getSystem<TerrainWorldSystem>() : nullptr;
    std::string why;
    TerrainWorldRuntime* runtime = system ? system->ensureRuntime(world, &why) : nullptr;
    if (runtime) {
        // R32's spacing and max height are the world's: the file's samples land one on each of its own
        char text[640];
        std::snprintf(text, sizeof(text),
                      "Reads a 16-bit RAW or R16 file a band of rows at a time, or a PNG or EXR whole (up to 8,193 a side), into the tiles it covers, "
                      "making the ones that are missing. Spacing (%.2f m) and Max Height (%.1f m) are the Terrain World's: one file sample "
                      "on each world sample, the file's lowest to highest value over 0 to Max Height. One undo step.",
                      runtime->lattice().spacing, runtime->maxHeight());
        EditorModal::message(text);
    } else {
        EditorModal::error(why.empty() ? std::string("The Terrain World is not running.") : why);
    }
    EditorModal::error(error);

    const float width = ImGui::GetFontSize() * 14.0f;
    ImGui::BeginDisabled(running);
    char file[512];
    std::snprintf(file, sizeof(file), "%s", path.c_str());
    ImGui::SetNextItemWidth(width);
    if (EditorModal::inputText("File", "path", file, sizeof(file))) path = file;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string());
        if (!chosen.empty()) path = FileUtils::pathToUtf8(project->normalizeToAssetsRelative(FileUtils::pathFromUtf8(chosen)));
    }
    ImGui::SetNextItemWidth(width * 0.5f);
    EditorModal::inputInt("Origin Tile X", "origin_x", &originX, 1, 8);
    ImGui::SetNextItemWidth(width * 0.5f);
    EditorModal::inputInt("Origin Tile Z", "origin_z", &originZ, 1, 8);
    originX = std::clamp(originX, WORLD_TILE_MIN, WORLD_TILE_MAX);
    originZ = std::clamp(originZ, WORLD_TILE_MIN, WORLD_TILE_MAX);
    EditorModal::checkbox("Flip Vertically", "flip_vertically", &flip);
    ImGui::SetNextItemWidth(width * 0.5f);
    EditorModal::inputInt("Raw Width", "width", &rawWidth, 1, 256);
    ImGui::SetNextItemWidth(width * 0.5f);
    EditorModal::inputInt("Raw Height", "height", &rawHeight, 1, 256);
    rawWidth = std::clamp(rawWidth, 0, 65537);
    rawHeight = std::clamp(rawHeight, 0, 65537);
    ImGui::TextDisabled("Raw Width and Height 0: from the file's length (a square)");
    ImGui::EndDisabled();

    TerrainWorldImportRequest request;
    request.path = path;
    request.width = rawWidth;
    request.height = rawHeight;
    request.flipVertically = flip;
    request.origin = TileCoord(originX, originZ);
    TerrainWorldImportPlan plan;
    std::string planError;
    const bool possible = runtime && !path.empty() && planTerrainWorldImport(project, *runtime, request, plan, planError);
    if (possible) {
        ImGui::TextDisabled("%i x %i samples%s: %i tiles from (%i, %i), %i new", plan.width, plan.height, plan.sizeGuessed ? " (from its length)" : "",
                            static_cast<int>(plan.covered.size()), originX, originZ, plan.made);
    } else if (!path.empty() && !planError.empty()) {
        EditorModal::error(planError);
    }
    if (running) {
        const double progress = task->progress();
        ImGui::ProgressBar(progress < 0.0 ? static_cast<float>(-1.0 * ImGui::GetTime()) : static_cast<float>(progress), ImVec2(width * 1.4f, 0),
                           task->phase().c_str());
    } else if (!done.empty()) {
        ImGui::TextUnformatted(done.c_str());
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!possible || running);
    if (EditorModal::button(running ? "Importing..." : "Import", "ok", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        error.clear();
        done.clear();
        task = startTerrainWorldImport(project, sceneId, world, plan, [this](const TerrainWorldImportResult& result) {
            if (result.ok) {
                done = "Imported into " + std::to_string(result.tiles) + " tile(s); Edit > Undo takes it back.";
                isOpen = false;
            } else {
                error = result.error;
            }
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(8.0f * ImGui::GetFontSize(), 0))) {
        if (running) {
            task->cancel();
        } else {
            isOpen = false;
        }
    }
    if (!isOpen) ImGui::CloseCurrentPopup();
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
        if (ImGui::Button("Import Heightmap...")) ImportHeightmapDialog::instance().open(project, sceneProject->id, entity);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A heightmap file into the tiles it covers, as a job you can cancel; one undo step.");
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
