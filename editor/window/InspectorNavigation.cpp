// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > NavMesh Surface / Agent / Obstacle / Link / Modifier and Nav Grid (spec S19 R32,
// R36), in Unity AI Navigation's layout: agent types and areas as dropdowns from the project's
// navigation settings (Window > AI > Navigation), the area mask as a checkbox list, the surface's
// overrides under an "Advanced" foldout with Bake / Clear, its stale-bake warning and a progress
// bar with Cancel, the agent's runtime state (read-only) in Play, and the grid's tilemap rect ->
// area table with Rebuild and Reset Overrides. Every value goes through the Catalog property
// (PropertyCmd), so undo, multi-selection and prefab overrides work as on any component.

#include "InspectorWindow.h"

#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavTypes.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeInput.h"
#include "reflect/Enums.h"
#include "subsystem/NavigationSystem.h"
#include "util/UIProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace jasperengine;

namespace {

    std::vector<editor::EnumEntry> entriesNeighbours = {
        {4, "4"},
        {8, "8"},
    };

    // The project's agent types and areas as combo entries, rebuilt from the settings each draw
    struct DynamicEntries {
        std::vector<std::string> names;
        std::vector<editor::EnumEntry> entries;
    };

    DynamicEntries& agentTypeEntries() {
        static DynamicEntries cache;
        const nav::NavigationSettings& settings = nav::getSettings();
        cache.names.clear();
        cache.entries.clear();
        cache.names.reserve(settings.agentTypes.size());
        for (const nav::AgentTypeSettings& agent : settings.agentTypes) cache.names.push_back(agent.name);
        for (size_t i = 0; i < settings.agentTypes.size(); i++)
            cache.entries.push_back({settings.agentTypes[i].id, cache.names[i].c_str()});
        return cache;
    }

    DynamicEntries& areaEntries() {
        static DynamicEntries cache;
        const nav::NavigationSettings& settings = nav::getSettings();
        cache.names.clear();
        cache.entries.clear();
        cache.names.reserve(nav::AREA_COUNT);
        for (int i = 0; i < nav::AREA_COUNT; i++) cache.names.push_back(settings.areas[(size_t)i].name);
        for (int i = 0; i < nav::AREA_COUNT; i++) cache.entries.push_back({i, cache.names[(size_t)i].c_str()});
        return cache;
    }

    std::string maskLabel(int mask) {
        const int all = nav::ALL_AREAS;
        if ((mask & all) == all) return "Everything";
        if ((mask & all) == 0) return "Nothing";
        const nav::NavigationSettings& settings = nav::getSettings();
        std::string label;
        int count = 0;
        for (int i = 0; i < nav::AREA_COUNT; i++) {
            if (!(mask & (1 << i))) continue;
            if (count++ < 3) label += (label.empty() ? "" : ", ") + settings.areas[(size_t)i].name;
        }
        if (count > 3) label = "Mixed (" + std::to_string(count) + ")";
        return label;
    }

    void warningLine(const char* text) {
        ImGui::TextColored(editor::Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    }

    const char* bakeStateName(NavBakeState state) {
        switch (state) {
            case NavBakeState::NOT_BAKED: return "Not baked";
            case NavBakeState::BAKED: return "Baked";
            case NavBakeState::DIRTY: return "Dirty";
            case NavBakeState::BAKING: return "Baking";
        }
        return "";
    }

}

// the area mask of agents: a dropdown of checkboxes over the project's areas (Unity's Area Mask)
void editor::InspectorWindow::navAreaMaskRow(ComponentType cpType, const std::string& id, const std::string& label,
                                             SceneProject* sceneProject, std::vector<Entity> entities, const char* help) {
    int mask = 0;
    bool mixed = false;
    for (size_t i = 0; i < entities.size(); i++) {
        PropertyData prop = Catalog::findProperty(sceneProject->scene, entities[i], cpType, id);
        if (!prop.ref) return;
        const int value = *static_cast<int*>(prop.ref);
        if (i == 0) mask = value;
        else if (value != mask) mixed = true;
    }
    const bool defChanged = (mask & nav::ALL_AREAS) != nav::ALL_AREAS;
    auto apply = [&](int value) {
        for (Entity entity : entities) {
            cmd = new PropertyCmd<int>(project, sceneProject->id, entity, cpType, id, value);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
        }
        finishProperty = true;
    };
    if (propertyHeader(label, -1, defChanged)) apply(nav::ALL_AREAS);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(("##navmask_" + id).c_str(), mixed ? "-" : maskLabel(mask).c_str())) {
        if (ImGui::Selectable("Nothing", false, ImGuiSelectableFlags_DontClosePopups)) apply(0);
        if (ImGui::Selectable("Everything", false, ImGuiSelectableFlags_DontClosePopups)) apply(nav::ALL_AREAS);
        ImGui::Separator();
        const nav::NavigationSettings& settings = nav::getSettings();
        for (int i = 0; i < nav::AREA_COUNT; i++) {
            bool on = (mask & (1 << i)) != 0;
            const std::string name = std::to_string(i) + "  " + settings.areas[(size_t)i].name + "##area" + std::to_string(i);
            if (ImGui::Checkbox(name.c_str(), &on)) apply(on ? (mask | (1 << i)) : (mask & ~(1 << i)));
        }
        ImGui::EndCombo();
    }
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
}

void editor::InspectorWindow::drawNavMeshSurfaceComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;

    RowSettings settingsAgent;
    settingsAgent.enumEntries = &agentTypeEntries().entries;
    settingsAgent.help = "The agent type it bakes for (Window > AI > Navigation > Agents): its radius, height, step and slope";
    RowSettings settingsCollect;
    settingsCollect.enumEntries = enumEntries(reflect::enums::kNavCollectObjects);
    settingsCollect.help = "What is collected: every static GameObject, the ones in the volume box, or this hierarchy";
    RowSettings settingsGeometry;
    settingsGeometry.enumEntries = enumEntries(reflect::enums::kNavUseGeometry);
    settingsGeometry.help = "Collect render meshes, collider shapes (no physics world needed) or both";
    RowSettings settingsLayers;
    settingsLayers.help = "GameObject layers collected";
    RowSettings settingsArea;
    settingsArea.enumEntries = &areaEntries().entries;
    settingsArea.help = "The area of what is collected unless a NavMesh Modifier says otherwise";
    RowSettings settingsPlane;
    settingsPlane.enumEntries = enumEntries(reflect::enums::kNavPlane);
    settingsPlane.help = "XZ for 3D scenes (Y up); XY for 2D scenes: Collider2D footprints on the scene's plane";
    RowSettings settingsCarving;
    settingsCarving.help = "Keep tile-cache layers so carving NavMesh Obstacles cut it at runtime (about 30% more memory)";
    RowSettings settingsData;
    settingsData.help = "The baked .navmesh asset";
    RowSettings settingsVolume;
    settingsVolume.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsAdvanced;
    settingsAdvanced.secondColSize = 6 * ImGui::GetFontSize();

    const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entities[0]);

    beginTable(cpType, getLabelSize("Runtime Carving"));
    propertyRow(RowPropertyType::Enum, cpType, "agentType", "Agent Type", sceneProject, entities, settingsAgent);
    propertyRow(RowPropertyType::Enum, cpType, "defaultArea", "Default Area", sceneProject, entities, settingsArea);
    propertyRow(RowPropertyType::Enum, cpType, "plane", "Plane", sceneProject, entities, settingsPlane);
    propertyRow(RowPropertyType::Bool, cpType, "runtimeCarving", "Runtime Carving", sceneProject, entities, settingsCarving);
    endTable();

    ImGui::SeparatorText("Object Collection");
    beginTable(cpType, getLabelSize("Include Layers"), "collect");
    propertyRow(RowPropertyType::Enum, cpType, "collectObjects", "Collect Objects", sceneProject, entities, settingsCollect);
    if (surface.collectObjects == NavCollectObjects::VOLUME) {
        RowSettings child = settingsVolume;
        child.indent = true;
        propertyRow(RowPropertyType::Vector3, cpType, "volumeCenter", "Center", sceneProject, entities, child);
        propertyRow(RowPropertyType::Vector3, cpType, "volumeSize", "Size", sceneProject, entities, child);
    }
    propertyRow(RowPropertyType::LayerMask, cpType, "includeLayers", "Include Layers", sceneProject, entities, settingsLayers);
    propertyRow(RowPropertyType::Enum, cpType, "useGeometry", "Use Geometry", sceneProject, entities, settingsGeometry);
    endTable();

    if (beginFoldout("Advanced##navsurface")) {
        const nav::AgentTypeSettings agent = nav::getSettings().agentTypeOrDefault(surface.agentType);
        beginTable(cpType, getLabelSize("Max Carving Obstacles"), "advanced");
        propertyRow(RowPropertyType::Bool, cpType, "overrideCellSize", "Override Voxel Size", sceneProject, entities, RowSettings());
        if (surface.overrideCellSize) {
            RowSettings child = settingsAdvanced;
            child.indent = true;
            child.format = "%.3f";
            child.stepSize = 0.01f;
            child.help = "Metres per voxel (default: the agent radius / 3)";
            propertyRow(RowPropertyType::FloatPositive, cpType, "cellSize", "Voxel Size", sceneProject, entities, child);
        }
        propertyRow(RowPropertyType::Bool, cpType, "overrideTileSize", "Override Tile Size", sceneProject, entities, RowSettings());
        if (surface.overrideTileSize) {
            RowSettings child = settingsAdvanced;
            child.indent = true;
            child.intMin = 16;
            child.intMax = 1024;
            child.help = "Voxels per tile side (carving: at most 255)";
            propertyRow(RowPropertyType::Int, cpType, "tileSize", "Tile Size", sceneProject, entities, child);
        }
        RowSettings regions = settingsAdvanced;
        regions.help = "Islands smaller than this (square metres) are dropped";
        propertyRow(RowPropertyType::FloatPositive, cpType, "minRegionArea", "Minimum Region Area", sceneProject, entities, regions);
        if (surface.runtimeCarving) {
            RowSettings obstacles = settingsAdvanced;
            obstacles.intMin = 1;
            obstacles.intMax = 4096;
            obstacles.help = "Carving NavMesh Obstacles it cuts at once; more are avoided instead, with a warning (read when the navmesh is created)";
            propertyRow(RowPropertyType::Int, cpType, "maxCarvingObstacles", "Max Carving Obstacles", sceneProject, entities, obstacles);
        }
        propertyRow(RowPropertyType::Asset, cpType, "navMeshData", "NavMesh Data", sceneProject, entities, settingsData);
        endTable();
        const float cellSize = surface.overrideCellSize ? surface.cellSize : agent.radius / 3.0f;
        const int tileSize = surface.overrideTileSize ? surface.tileSize : 64;
        ImGui::TextDisabled("%.3f m voxels, tiles of %.1f m; %d voxels per agent radius", cellSize, cellSize * (float)tileSize,
                            (int)std::ceil(agent.radius / std::max(cellSize, 1e-4f)));
        // a 2D surface works in points: the same sizes at the scene's points per metre
        if (surface.plane == nav::NavPlane::XY) {
            const float points = nav::NavBakeCollector::unitScaleOf(scene, surface);
            ImGui::TextDisabled("2D: %.1f point voxels, tiles of %.0f points", cellSize * points, cellSize * (float)tileSize * points);
        }
        endFoldout();
    }

    if (entities.size() != 1) return;
    const Entity entity = entities[0];
    NavigationSystem* system = NavigationEditor::systemOf(*sceneProject);
    if (!system) return;
    if (!nav::isNavMeshAvailable()) {
        warningLine("This editor was built without Recast/Detour (JASPERENGINE_NAVIGATION=OFF): navmeshes cannot be baked.");
        return;
    }

    const NavSurfaceStatus status = system->getSurfaceStatus(entity);
    NavigationEditor& navEditor = NavigationEditor::get();
    const bool baking = status.state == NavBakeState::BAKING || navEditor.isBaking(sceneProject->id, entity);
    // the summary lines are rows get_inspector_layout reads (kind Label, their text as label)
    auto infoRow = [&](const char* id, const std::string& text) {
        recordLayoutRow(RowPropertyType::Label, cpType, id, text, sceneProject, entities);
    };

    // S75 R29: what a bake would build, from the editor's cached collection (Volume: the box)
    ImGui::Spacing();
    {
        // the bake's own configuration (nav units: metres, scene points on a 2D surface)
        const nav::NavBuildConfig config = nav::NavBakeCollector::configOf(scene, surface);
        float cellSize = config.cellSize;
        int tileSize = config.tileSize;
        int64_t wide = -1, high = -1;
        if (surface.collectObjects == NavCollectObjects::VOLUME) {
            const Transform* transform = scene->findComponent<Transform>(entity);
            const Vector3 scale = transform ? transform->worldScale : Vector3(1, 1, 1);
            const float depth = surface.plane == nav::NavPlane::XY ? surface.volumeSize.y * std::fabs(scale.y) : surface.volumeSize.z * std::fabs(scale.z);
            wide = (int64_t)std::ceil(surface.volumeSize.x * std::fabs(scale.x) / std::max(cellSize, 1e-6f));
            high = (int64_t)std::ceil(depth / std::max(cellSize, 1e-6f));
            wide = (wide + tileSize - 1) / tileSize * tileSize;
            high = (high + tileSize - 1) / tileSize * tileSize;
        } else if (const NavigationSystem::InputReport* report = navEditor.inputReport(*sceneProject, entity)) {
            if (report->tileSize > 0) {
                cellSize = report->cellSize;
                tileSize = report->tileSize;
                wide = (int64_t)report->tilesWide * report->tileSize;
                high = (int64_t)report->tilesHigh * report->tileSize;
            } else if (!report->error.empty()) {
                const std::string text = "A bake would fail: " + report->error + " (" + nav::bakeErrorName(report->errorCode) + ")";
                warningLine(text.c_str());
                infoRow("estimateError", text);
            }
        }
        if (wide >= 0 && high >= 0) {
            const int64_t tiles = (wide / std::max(tileSize, 1)) * (high / std::max(tileSize, 1));
            const float perRadius = config.agentRadius / std::max(cellSize, 1e-6f);
            char estimate[160];
            std::snprintf(estimate, sizeof(estimate), "Voxels %lld \xC3\x97 %lld, %lld tiles (%.1f voxels per agent radius)", (long long)wide,
                          (long long)high, (long long)tiles, perRadius);
            ImGui::TextDisabled("%s", estimate);
            ImGui::SetItemTooltip("What a bake builds: the voxel grid of the collected bounds, its tiles, and how finely the agent's "
                                  "radius is voxelized");
            infoRow("estimate", estimate);
            if (perRadius < 2.0f) {
                const std::string text = "Fewer than 2 voxels per agent radius: corners and narrow passages get lost. Lower the Voxel Size "
                                         "(Advanced) to radius / 3.";
                warningLine(text.c_str());
                infoRow("estimateVoxels", text);
            }
            if (tiles > 512) {
                const std::string text = std::to_string(tiles) + " tiles is above the mobile/web budget (512): raise the Tile Size or the "
                                         "Voxel Size, or bake a Volume.";
                warningLine(text.c_str());
                infoRow("estimateTiles", text);
            }
            const std::string limit = nav::checkPolyRefBits(tiles, config.maxPolysPerTile);
            if (!limit.empty()) {
                const std::string text = "The bake would be refused: " + limit;
                warningLine(text.c_str());
                infoRow("estimateLimit", text);
            }
        }
    }
    if (baking) {
        const std::string overlay = "Baking " + std::to_string((int)std::round(status.progress * 100.0f)) + "%";
        ImGui::ProgressBar(status.progress, ImVec2(-6.0f * ImGui::GetFontSize(), 0.0f), overlay.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel##navbake", ImVec2(-1, 0))) navEditor.cancel(project, sceneProject->id, entity);
        return;
    }

    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;
    ImGui::BeginDisabled(playing);
    // (a refused start is logged by NavigationEditor; a Shift-click starts only the full bake
    // below, so it is logged once)
    std::string error;
    if (ImGui::Button(ICON_FA_HAMMER " Bake") && !ImGui::GetIO().KeyShift) {
        // a stale bake rebuilds the changed tiles; nothing baked: every tile
        navEditor.bake(project, sceneProject->id, entity, NavBakeMode::DIRTY, error);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Bakes the tiles whose inputs changed (all of them the first time); Shift: every tile");
    if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift) {
        navEditor.cancel(project, sceneProject->id, entity);
        navEditor.bake(project, sceneProject->id, entity, NavBakeMode::FULL, error);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(status.state == NavBakeState::NOT_BAKED && surface.navMeshData.empty());
    if (ImGui::Button(ICON_FA_TRASH_CAN " Clear")) navEditor.clear(project, sceneProject->id, entity, error);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // the last bake of this session (S74 R3): a failure until a bake succeeds, or its warnings
    const NavigationEditor::LastBake* editorBake = navEditor.lastBake(sceneProject->id, entity);
    const bool failureShown = editorBake && !editorBake->ok && !editorBake->cancelled;
    if (failureShown) {
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::ErrorText);
        ImGui::TextWrapped("Bake failed: %s", editorBake->error.c_str());
        ImGui::PopStyleColor();
    } else if (editorBake && editorBake->ok && !editorBake->warnings.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::WarningText);
        ImGui::TextWrapped("Baked with %d warning(s); see the Console", (int)editorBake->warnings.size());
        ImGui::PopStyleColor();
    }

    // the state line, then the stale-bake warning (R32)
    if (status.state == NavBakeState::NOT_BAKED) {
        if (!status.loadError.empty()) {
            warningLine(("Not baked: " + status.loadError).c_str());
        } else {
            ImGui::TextDisabled("Not baked");
        }
    } else {
        ImGui::TextDisabled("%s: %d tiles, %d polys, %.1f KB%s", bakeStateName(status.state), status.tiles, status.polys,
                            (double)status.bytes / 1024.0, status.carving ? ", carving" : "");
        if (!playing) {
            const int dirty = navEditor.dirtyTiles(project, sceneProject->id, entity);
            if (dirty > 0) {
                warningLine(("The baked navmesh is out of date: " + std::to_string(dirty) +
                             " tile(s) changed since the bake. Bake to update them.").c_str());
            }
        }
    }
    // S75 R29: the last bake of this session, the walkable area and the islands
    if (const NavBakeResult* last = system->getLastBakeResult(entity)) {
        char line[192];
        if (last->ok) {
            std::snprintf(line, sizeof(line), "Last bake: %.0f ms, %d of %d tiles built, %d polys", last->milliseconds, last->tilesBuilt,
                          last->tilesTotal, last->polys);
            ImGui::TextDisabled("%s", line);
        } else {
            std::snprintf(line, sizeof(line), "Last bake %s: %s", last->cancelled ? "cancelled" : "failed",
                          last->error.empty() ? nav::bakeErrorName(last->errorCode) : last->error.c_str());
            ImGui::PushTextWrapPos(0.0f);
            // (the same failure is the "Bake failed" line above already: only its hint here)
            if (!(failureShown && editorBake->error == last->error)) ImGui::TextColored(Theme::Colors::ErrorText, "%s", line);
            if (!last->hint.empty()) ImGui::TextDisabled("%s", last->hint.c_str());
            ImGui::PopTextWrapPos();
        }
        infoRow("lastBake", line);
        if (!last->warnings.empty()) {
            const std::string label = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(last->warnings.size()) +
                                      (last->warnings.size() == 1 ? " warning" : " warnings") + "##navbakewarnings";
            infoRow("lastBakeWarnings", label.substr(0, label.find("##")));
            if (ImGui::TreeNode(label.c_str())) {
                ImGui::PushTextWrapPos(0.0f);
                for (const std::string& warning : last->warnings) ImGui::TextColored(Theme::Colors::WarningText, "%s", warning.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TreePop();
            }
        }
    }
    if (status.state != NavBakeState::NOT_BAKED) {
        const nav::NavCoverage coverage = system->getCoverage(entity);
        char line[160];
        std::snprintf(line, sizeof(line), "Walkable: %.1f m\xC2\xB2, %d island%s (largest %.1f m\xC2\xB2)", coverage.walkableArea,
                      coverage.islandCount, coverage.islandCount == 1 ? "" : "s", coverage.largestIslandArea);
        ImGui::TextDisabled("%s", line);
        ImGui::SetItemTooltip("The navmesh's area and its connected parts (links join them); Scene view > AI Navigation > Color By "
                              "Islands shows them");
        infoRow("walkable", line);
    }
    if (playing && status.crowdCapacity > 0)
        ImGui::TextDisabled("Crowd: %d / %d agents", status.crowdAgents, status.crowdCapacity);
}

void editor::InspectorWindow::drawNavMeshAgentComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsAgent;
    settingsAgent.enumEntries = &agentTypeEntries().entries;
    settingsAgent.help = "The agent type (Window > AI > Navigation > Agents): it walks the surfaces baked for it";
    RowSettings settingsOffset = settings;
    settingsOffset.help = "Height of the pivot above the navmesh";
    RowSettings settingsSpeed = settings;
    settingsSpeed.help = "Top speed (m/s; metres on 2D surfaces too)";
    RowSettings settingsAngular = settings;
    settingsAngular.help = "Turn rate (degrees/s) while Update Rotation is on";
    RowSettings settingsAccel = settings;
    settingsAccel.help = "m/s2 to speed up and to brake";
    RowSettings settingsStop = settings;
    settingsStop.help = "It stops this far from the destination";
    RowSettings settingsBraking;
    settingsBraking.help = "Slow down before the destination so it stops there without overshooting";
    RowSettings settingsAvoidance;
    settingsAvoidance.enumEntries = enumEntries(reflect::enums::kNavAvoidanceQuality);
    settingsAvoidance.help = "How carefully it steers around other agents (None: it walks through them)";
    RowSettings settingsPriority = settings;
    settingsPriority.intMin = 0;
    settingsPriority.intMax = 99;
    settingsPriority.help = "0-99, lower is more important: it avoids agents of its priority or lower and ignores higher numbers";
    RowSettings settingsDriver;
    settingsDriver.enumEntries = enumEntries(reflect::enums::kNavAgentDriver);
    settingsDriver.help = "What moves it: its Transform, its Rigidbody's velocity, its Character Controller, or nothing (scripts)";
    RowSettings settingsSurface;
    settingsSurface.help = "The NavMesh Surface or Nav Grid it walks; none: the surface of its type under it, else a grid holding it";
    RowSettings settingsFlag;

    beginTable(cpType, getLabelSize("Auto Traverse Off Mesh Link"));
    propertyRow(RowPropertyType::Enum, cpType, "agentType", "Agent Type", sceneProject, entities, settingsAgent);
    propertyRow(RowPropertyType::Float, cpType, "baseOffset", "Base Offset", sceneProject, entities, settingsOffset);
    endTable();

    ImGui::SeparatorText("Steering");
    beginTable(cpType, getLabelSize("Auto Traverse Off Mesh Link"), "steering");
    propertyRow(RowPropertyType::FloatPositive, cpType, "speed", "Speed", sceneProject, entities, settingsSpeed);
    propertyRow(RowPropertyType::FloatPositive, cpType, "angularSpeed", "Angular Speed", sceneProject, entities, settingsAngular);
    propertyRow(RowPropertyType::FloatPositive, cpType, "acceleration", "Acceleration", sceneProject, entities, settingsAccel);
    propertyRow(RowPropertyType::FloatPositive, cpType, "stoppingDistance", "Stopping Distance", sceneProject, entities, settingsStop);
    propertyRow(RowPropertyType::Bool, cpType, "autoBraking", "Auto Braking", sceneProject, entities, settingsBraking);
    endTable();

    ImGui::SeparatorText("Obstacle Avoidance");
    beginTable(cpType, getLabelSize("Auto Traverse Off Mesh Link"), "avoidance");
    propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Enum, cpType, "obstacleAvoidance", "Quality", sceneProject, entities, settingsAvoidance);
    propertyRow(RowPropertyType::Int, cpType, "avoidancePriority", "Priority", sceneProject, entities, settingsPriority);
    endTable();

    ImGui::SeparatorText("Path Finding");
    beginTable(cpType, getLabelSize("Auto Traverse Off Mesh Link"), "pathfinding");
    propertyRow(RowPropertyType::Bool, cpType, "autoTraverseOffMeshLink", "Auto Traverse Off Mesh Link", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "autoRepath", "Auto Repath", sceneProject, entities, settingsFlag);
    navAreaMaskRow(cpType, "areaMask", "Area Mask", sceneProject, entities, "The areas its paths may enter");
    propertyRow(RowPropertyType::LocalEntity, cpType, "surface", "Surface", sceneProject, entities, settingsSurface);
    propertyRow(RowPropertyType::Enum, cpType, "driver", "Driver", sceneProject, entities, settingsDriver);
    propertyRow(RowPropertyType::Bool, cpType, "updatePosition", "Update Position", sceneProject, entities, settingsFlag);
    propertyRow(RowPropertyType::Bool, cpType, "updateRotation", "Update Rotation", sceneProject, entities, settingsFlag);
    endTable();

    // ---- S76 agents: the stuck watchdog (R40)
    if (beginFoldout("Stuck Detection##navagent")) {
        RowSettings settingsDetect;
        settingsDetect.help = "Fire onStuck when it makes no progress along its path for Stuck After seconds";
        RowSettings settingsRatio = settings;
        settingsRatio.help = "Progress counts once it moved this fraction of Speed x Stuck After from where it was";
        RowSettings settingsTimeout = settings;
        settingsTimeout.help = "Seconds without progress before onStuck fires";
        RowSettings settingsRepath;
        settingsRepath.help = "Ask for the destination again when stuck";
        beginTable(cpType, getLabelSize("Repath When Stuck"), "stuck");
        propertyRow(RowPropertyType::Bool, cpType, "stuckDetection", "Detect Stuck", sceneProject, entities, settingsDetect);
        propertyRow(RowPropertyType::FloatPositive, cpType, "stuckSpeedRatio", "Stuck Speed Ratio", sceneProject, entities, settingsRatio);
        propertyRow(RowPropertyType::FloatPositive, cpType, "stuckTimeout", "Stuck After (s)", sceneProject, entities, settingsTimeout);
        propertyRow(RowPropertyType::Bool, cpType, "repathWhenStuck", "Repath When Stuck", sceneProject, entities, settingsRepath);
        endTable();
    }

    // what the crowd says, while the scene plays (R32)
    if (sceneProject->playState != ScenePlayState::STOPPED && entities.size() == 1) {
        const NavMeshAgentComponent& agent = scene->getComponent<NavMeshAgentComponent>(entities[0]);
        ImGui::SeparatorText("Runtime");
        ImGui::TextDisabled("Path Status: %s%s", nav::pathStatusName(agent.pathStatus), agent.pathPending ? " (pending)" : "");
        ImGui::TextDisabled("Remaining Distance: %.2f", agent.remainingDistance);
        ImGui::TextDisabled("Is On NavMesh: %s", agent.isOnNavMesh ? "yes" : "no");
        ImGui::TextDisabled("Velocity: %.2f, %.2f, %.2f", agent.velocity.x, agent.velocity.y, agent.velocity.z);
        ImGui::TextDisabled("Destination: %.2f, %.2f, %.2f", agent.destination.x, agent.destination.y, agent.destination.z);
        // ---- S76 agents (R26, R27, R21, R9)
        if (agent.stuckDetection)
            ImGui::TextDisabled("Stuck Time: %.2f s", agent.stuckTime);
        ImGui::TextDisabled("Last Path Failure: %s", nav::pathFailureName(agent.lastPathFailure));
        if (NavigationSystem* system = scene->getSystem<NavigationSystem>().get()) {
            ImGui::TextDisabled("Path Stale: %s", system->isAgentPathStale(entities[0]) ? "yes" : "no");
            const nav::NavigationSettings& navSettings = nav::getSettings();
            for (int area = 0; area < nav::AREA_COUNT; area++) {
                const float cost = system->getAgentAreaCost(entities[0], area);
                if (cost != navSettings.areas[(size_t)area].cost)
                    ImGui::TextDisabled("Area Cost: %s %.2f", navSettings.areas[(size_t)area].name.c_str(), cost);
            }
        }
    }
}

void editor::InspectorWindow::drawNavMeshObstacleComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;
    const NavMeshObstacleComponent& obstacle = scene->getComponent<NavMeshObstacleComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsShape;
    settingsShape.enumEntries = enumEntries(reflect::enums::kNavObstacleShape);
    RowSettings settingsCarve;
    settingsCarve.help = "Cut a hole in the navmesh (needs Runtime Carving on its surface); off: agents steer around it";
    RowSettings settingsMove = settings;
    settingsMove.help = "It is carved again once it moved this far";
    RowSettings settingsStationary = settings;
    settingsStationary.help = "Seconds without moving before it counts as stationary";
    RowSettings settingsOnly;
    settingsOnly.help = "Carve only while stationary (moving, it is avoided)";

    beginTable(cpType, getLabelSize("Carve Only Stationary"));
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Shape", sceneProject, entities, settingsShape);
    propertyRow(RowPropertyType::Vector3, cpType, "center", "Center", sceneProject, entities, settings);
    if (obstacle.shape == NavObstacleShape::BOX) {
        propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settings);
    } else {
        propertyRow(RowPropertyType::FloatPositive, cpType, "radius", "Radius", sceneProject, entities, settings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "height", "Height", sceneProject, entities, settings);
    }
    propertyRow(RowPropertyType::Bool, cpType, "carve", "Carve", sceneProject, entities, settingsCarve);
    if (obstacle.carve) {
        settingsMove.indent = true;
        settingsStationary.indent = true;
        settingsOnly.indent = true;
        propertyRow(RowPropertyType::FloatPositive, cpType, "moveThreshold", "Move Threshold", sceneProject, entities, settingsMove);
        propertyRow(RowPropertyType::FloatPositive, cpType, "timeToStationary", "Time To Stationary", sceneProject, entities, settingsStationary);
        propertyRow(RowPropertyType::Bool, cpType, "carveOnlyStationary", "Carve Only Stationary", sceneProject, entities, settingsOnly);
    }
    endTable();
}

void editor::InspectorWindow::drawNavMeshLinkComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsAgent;
    settingsAgent.enumEntries = &agentTypeEntries().entries;
    RowSettings settingsPoint = settings;
    settingsPoint.help = "In this GameObject's space; drag the handles in the Scene view";
    RowSettings settingsWidth = settings;
    settingsWidth.help = "0: one point-to-point connection; wider: connections side by side, one per agent diameter";
    RowSettings settingsCost = settings;
    settingsCost.help = "Negative: the area's cost; otherwise the cost per metre of this link";
    RowSettings settingsBidirectional;
    settingsBidirectional.help = "Agents may cross it both ways";
    RowSettings settingsAuto;
    settingsAuto.help = "Follow the GameObject when it moves (the link is updated at runtime)";
    RowSettings settingsArea;
    settingsArea.enumEntries = &areaEntries().entries;
    RowSettings settingsActivated;
    settingsActivated.help = "Agents may use it; off: paths and agents ignore it (an agent on it finishes). Not baked";

    beginTable(cpType, getLabelSize("Auto Update Positions"));
    propertyRow(RowPropertyType::Enum, cpType, "agentType", "Agent Type", sceneProject, entities, settingsAgent);
    propertyRow(RowPropertyType::Vector3, cpType, "startPoint", "Start Point", sceneProject, entities, settingsPoint);
    propertyRow(RowPropertyType::Vector3, cpType, "endPoint", "End Point", sceneProject, entities, settingsPoint);
    propertyRow(RowPropertyType::FloatPositive, cpType, "width", "Width", sceneProject, entities, settingsWidth);
    propertyRow(RowPropertyType::Float, cpType, "costModifier", "Cost Override", sceneProject, entities, settingsCost);
    propertyRow(RowPropertyType::Bool, cpType, "autoUpdatePosition", "Auto Update Positions", sceneProject, entities, settingsAuto);
    propertyRow(RowPropertyType::Bool, cpType, "bidirectional", "Bidirectional", sceneProject, entities, settingsBidirectional);
    propertyRow(RowPropertyType::Enum, cpType, "area", "Area Type", sceneProject, entities, settingsArea);
    propertyRow(RowPropertyType::Bool, cpType, "activated", "Activated", sceneProject, entities, settingsActivated);
    endTable();

    // ---- S76 areas: in play, whether an agent is crossing it (R17)
    NavigationSystem* system = NavigationEditor::systemOf(*sceneProject);
    if (entities.size() == 1 && system && sceneProject->playState != ScenePlayState::STOPPED)
        ImGui::TextDisabled("Occupied: %s", system->isLinkOccupied(entities[0]) ? "yes" : "no");

    if (entities.size() == 1) {
        const NavMeshLinkComponent& link = scene->getComponent<NavMeshLinkComponent>(entities[0]);
        if (ImGui::Button(ICON_FA_ARROWS_LEFT_RIGHT " Swap")) {
            Vector3 start = link.startPoint;
            Vector3 end = link.endPoint;
            auto multi = new MultiPropertyCmd();
            multi->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "startPoint", end);
            multi->addPropertyCmd<Vector3>(project, sceneProject->id, entities[0], cpType, "endPoint", start);
            CommandHandle::get(project->getSelectedSceneId())->addCommand(multi);
            multi->setNoMerge();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swaps the start and end points");
    }
}

void editor::InspectorWindow::drawNavMeshModifierComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;
    const NavMeshModifierComponent& modifier = scene->getComponent<NavMeshModifierComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsInclusion;
    settingsInclusion.enumEntries = enumEntries(reflect::enums::kNavModifierInclusion);
    settingsInclusion.help = "Surface Policy: collected as the surface decides (moving bodies, agents and obstacles are left out); "
                             "Add or Modify Object: collected even when the surface would leave it out; Remove Object: never collected";
    RowSettings settingsOverride;
    settingsOverride.help = "Give what it covers its own area";
    RowSettings settingsArea;
    settingsArea.enumEntries = &areaEntries().entries;
    settingsArea.indent = true;
    RowSettings settingsChildren;
    settingsChildren.help = "Its descendants too (the nearest modifier wins)";
    RowSettings settingsVolume;
    settingsVolume.help = "Mark a box (this GameObject's space) instead of the GameObject's geometry";
    RowSettings settingsRuntime;
    settingsRuntime.indent = true;
    settingsRuntime.help = "The bake only splits the navmesh along the volume; while enabled, the polygons inside take its Area Type "
                           "(a door, a bridge), in edit mode and in play";
    // a runtime volume always marks its Area Type (S76 R40)
    const bool runtimeVolume = modifier.useVolume && modifier.applyAtRuntime;

    beginTable(cpType, getLabelSize("Apply At Runtime"));
    propertyRow(RowPropertyType::Enum, cpType, "overrideInclusion", "Mode", sceneProject, entities, settingsInclusion);
    propertyRow(RowPropertyType::Bool, cpType, "overrideArea", "Override Area", sceneProject, entities, settingsOverride);
    if (modifier.overrideArea || runtimeVolume) propertyRow(RowPropertyType::Enum, cpType, "area", "Area Type", sceneProject, entities, settingsArea);
    propertyRow(RowPropertyType::Bool, cpType, "applyToChildren", "Apply To Children", sceneProject, entities, settingsChildren);
    propertyRow(RowPropertyType::Bool, cpType, "useVolume", "Volume", sceneProject, entities, settingsVolume);
    if (modifier.useVolume) {
        RowSettings child = settings;
        child.indent = true;
        propertyRow(RowPropertyType::Vector3, cpType, "volumeCenter", "Center", sceneProject, entities, child);
        propertyRow(RowPropertyType::Vector3, cpType, "volumeSize", "Size", sceneProject, entities, child);
        propertyRow(RowPropertyType::Bool, cpType, "applyAtRuntime", "Apply At Runtime", sceneProject, entities, settingsRuntime);
    }
    endTable();

    // ---- S76 areas: in play, the polygons the runtime volume holds now
    NavigationSystem* system = NavigationEditor::systemOf(*sceneProject);
    if (entities.size() == 1 && runtimeVolume && system && sceneProject->playState != ScenePlayState::STOPPED)
        ImGui::TextDisabled("Applied Polygons: %d", system->getModifierAppliedPolygons(entities[0]));
}

void editor::InspectorWindow::drawNavGridComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;
    const NavGridComponent& grid = scene->getComponent<NavGridComponent>(entities[0]);

    RowSettings settings;
    settings.secondColSize = 6 * ImGui::GetFontSize();
    RowSettings settingsLayout;
    settingsLayout.enumEntries = enumEntries(reflect::enums::kNavGridLayout);
    settingsLayout.help = "Square cells, or hex cells (pointy-top, odd rows shifted right by half a cell)";
    RowSettings settingsNeighbours;
    settingsNeighbours.enumEntries = &entriesNeighbours;
    settingsNeighbours.help = "Square grids: 4 (no diagonals) or 8 neighbours";
    RowSettings settingsSize = settings;
    settingsSize.intMin = 1;
    settingsSize.intMax = 4096;
    RowSettings settingsPlane;
    settingsPlane.enumEntries = enumEntries(reflect::enums::kNavPlane);
    RowSettings settingsSlope = settings;
    settingsSlope.help = "With heights (Terrain source): steps steeper than this (degrees) are blocked";
    RowSettings settingsStep = settings;
    settingsStep.help = "With heights: steps higher than this are blocked";
    RowSettings settingsMovement;
    settingsMovement.enumEntries = enumEntries(reflect::enums::kNavGridMovement);
    settingsMovement.help = "Free: straight lines between turns (line of sight); Cell Centres: from centre to centre";
    RowSettings settingsLookahead = settings;
    settingsLookahead.help = "Seconds of their path agents reserve ahead; others pay the penalty on reserved cells";
    RowSettings settingsPenalty = settings;
    settingsPenalty.help = "Cost multiplier of a cell another agent reserved";
    RowSettings settingsSource;
    settingsSource.enumEntries = enumEntries(reflect::enums::kNavGridSource);
    settingsSource.help = "Where Rebuild takes the cells from: painted only, a Tilemap's tiles or a Terrain's heights";
    RowSettings settingsSourceEntity;
    settingsSourceEntity.help = "The Tilemap or Terrain; none: this GameObject's";
    RowSettings settingsEmpty;
    settingsEmpty.enumEntries = &areaEntries().entries;
    settingsEmpty.help = "The area of cells no tile covers";
    RowSettings settingsFlag;

    beginTable(cpType, getLabelSize("Reservation Lookahead"));
    propertyRow(RowPropertyType::Enum, cpType, "layout", "Layout", sceneProject, entities, settingsLayout);
    if (grid.layout == nav::GridLayout::Square)
        propertyRow(RowPropertyType::Enum, cpType, "neighbours", "Neighbours", sceneProject, entities, settingsNeighbours);
    propertyRow(RowPropertyType::FloatPositive, cpType, "cellSize", "Cell Size", sceneProject, entities, settings);
    propertyRow(RowPropertyType::Int, cpType, "width", "Width", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::Int, cpType, "height", "Height", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::Enum, cpType, "plane", "Plane", sceneProject, entities, settingsPlane);
    propertyRow(RowPropertyType::Enum, cpType, "movement", "Movement", sceneProject, entities, settingsMovement);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxSlope", "Max Slope", sceneProject, entities, settingsSlope);
    propertyRow(RowPropertyType::FloatPositive, cpType, "maxStep", "Max Step", sceneProject, entities, settingsStep);
    propertyRow(RowPropertyType::FloatPositive, cpType, "reservationLookahead", "Reservation Lookahead", sceneProject, entities, settingsLookahead);
    propertyRow(RowPropertyType::FloatPositive, cpType, "reservationPenalty", "Reservation Penalty", sceneProject, entities, settingsPenalty);
    endTable();

    ImGui::SeparatorText("Source");
    beginTable(cpType, getLabelSize("Cells Without Tiles"), "source");
    propertyRow(RowPropertyType::Enum, cpType, "source", "Source", sceneProject, entities, settingsSource);
    if (grid.source != NavGridSource::NONE) {
        propertyRow(RowPropertyType::LocalEntity, cpType, "sourceEntity", "Source Object", sceneProject, entities, settingsSourceEntity);
        propertyRow(RowPropertyType::Bool, cpType, "autoRebuild", "Auto Rebuild", sceneProject, entities, settingsFlag);
    }
    if (grid.source == NavGridSource::TILEMAP) {
        propertyRow(RowPropertyType::Enum, cpType, "emptyCellArea", "Cells Without Tiles", sceneProject, entities, settingsEmpty);
        RowSettings colliders;
        colliders.help = "Static Collider2D footprints block the cells under them";
        propertyRow(RowPropertyType::Bool, cpType, "collidersBlock", "Colliders Block", sceneProject, entities, colliders);
    }
    propertyRow(RowPropertyType::Asset, cpType, "gridData", "Grid Data", sceneProject, entities, RowSettings());
    endTable();

    if (entities.size() != 1) return;
    const Entity entity = entities[0];

    // the tilemap's rects -> areas (R36)
    if (grid.source == NavGridSource::TILEMAP) {
        const Entity tilemapEntity = grid.sourceEntity != NULL_ENTITY ? grid.sourceEntity : entity;
        const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(tilemapEntity);
        if (!tilemap) {
            warningLine("No Tilemap on the source object.");
        } else if (ImGui::BeginTable("##navtileareas", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Tile Rect");
            ImGui::TableSetupColumn("Area");
            ImGui::TableHeadersRow();
            DynamicEntries& areas = areaEntries();
            for (unsigned int r = 0; r < tilemap->numTilesRect; r++) {
                const std::string& rectName = tilemap->tilesRect[r].name;
                if (rectName.empty()) continue;
                int area = nav::AREA_WALKABLE;
                for (const NavGridTileArea& entry : grid.tileAreas) {
                    if (entry.rect == rectName) area = entry.area;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rectName.c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                const int clamped = std::clamp(area, 0, nav::AREA_COUNT - 1);
                if (ImGui::BeginCombo(("##tilearea" + std::to_string(r)).c_str(), areas.names[(size_t)clamped].c_str())) {
                    for (int a = 0; a < nav::AREA_COUNT; a++) {
                        if (ImGui::Selectable(areas.names[(size_t)a].c_str(), a == clamped) && a != area) {
                            std::vector<NavGridTileArea> table = grid.tileAreas;
                            bool found = false;
                            for (NavGridTileArea& entry : table) {
                                if (entry.rect == rectName) {
                                    entry.area = a;
                                    found = true;
                                }
                            }
                            if (!found) table.push_back({rectName, a});
                            cmd = new PropertyCmd<std::vector<NavGridTileArea>>(project, sceneProject->id, entity, cpType, "tileAreas", table);
                            CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
                            cmd->setNoMerge();
                            finishProperty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::EndTable();
        }
    }

    NavigationSystem* system = NavigationEditor::systemOf(*sceneProject);
    nav::NavGridMap* map = system ? system->getGridMap(entity) : nullptr;
    if (!map) return;
    ImGui::BeginDisabled(grid.source == NavGridSource::NONE);
    if (ImGui::Button(ICON_FA_ROTATE " Rebuild")) {
        NavigationEditor::get().rebuildGrid(project, sceneProject->id, entity);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Takes the cells from the source again; brush-painted cells are kept (overrides)");
    ImGui::SameLine();
    const int overrides = map->countOverrides();
    ImGui::BeginDisabled(overrides == 0);
    if (ImGui::Button("Reset Overrides")) {
        NavigationEditor::get().resetGridOverrides(project, sceneProject->id, entity);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    NavigationEditor& navEditor = NavigationEditor::get();
    const bool brushHere = navEditor.isBrushActive(sceneProject->id) && navEditor.getBrushGrid() == entity;
    if (!brushHere) {
        ImGui::BeginDisabled(sceneProject->playState != ScenePlayState::STOPPED);
        if (ImGui::Button(ICON_FA_PAINTBRUSH " Paint")) navEditor.setBrushGrid(sceneProject->id, entity);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The nav-grid brush in the Scene view: paint, erase or fill areas ([ and ] resize, Shift-drag a line)");
    } else {
        if (ImGui::Button(ICON_FA_STOP " Stop Painting")) navEditor.stopBrush();
        NavigationEditor::Brush& brush = navEditor.getBrush();
        ImGui::SeparatorText("Nav-Grid Brush");
        int mode = (int)brush.mode;
        ImGui::RadioButton("Paint Area", &mode, (int)NavigationEditor::BrushMode::Paint);
        ImGui::SameLine();
        ImGui::RadioButton("Erase", &mode, (int)NavigationEditor::BrushMode::Erase);
        ImGui::SameLine();
        ImGui::RadioButton("Fill", &mode, (int)NavigationEditor::BrushMode::Fill);
        brush.mode = (NavigationEditor::BrushMode)mode;
        if (brush.mode == NavigationEditor::BrushMode::Paint || brush.mode == NavigationEditor::BrushMode::Fill) {
            DynamicEntries& areas = areaEntries();
            const int current = std::clamp(brush.area, 0, nav::AREA_COUNT - 1);
            ImGui::SetNextItemWidth(12 * ImGui::GetFontSize());
            if (ImGui::BeginCombo("Area##navbrush", areas.names[(size_t)current].c_str())) {
                for (int a = 0; a < nav::AREA_COUNT; a++) {
                    if (ImGui::Selectable(areas.names[(size_t)a].c_str(), a == current)) brush.area = a;
                }
                ImGui::EndCombo();
            }
        }
        if (brush.mode != NavigationEditor::BrushMode::Fill) {
            int shape = (int)brush.shape;
            ImGui::RadioButton("Circle", &shape, (int)NavigationEditor::BrushShape::Circle);
            ImGui::SameLine();
            ImGui::RadioButton("Square", &shape, (int)NavigationEditor::BrushShape::Square);
            brush.shape = (NavigationEditor::BrushShape)shape;
            ImGui::SetNextItemWidth(12 * ImGui::GetFontSize());
            ImGui::SliderInt("Size (cells)##navbrush", &brush.size, 1, 64);
            // the size in the name: a UI test reads what [ and ] did
            UIProbe::item("inspector.navBrushSize." + std::to_string(brush.size));
        }
        ImGui::TextDisabled("Left-drag in the Scene view paints; one stroke is one undo step.");
    }
    ImGui::TextDisabled("%d x %d cells, %d painted override(s), %.1f KB", map->getWidth(), map->getHeight(), overrides,
                        (double)map->memoryBytes() / 1024.0);
}
