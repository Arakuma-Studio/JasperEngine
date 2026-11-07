// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > Voxel Volume (docs/specs/T6-voxel-volumes.md R1, R61, R65), with Unity-style labels: the
// error strip, Edit Volume, Voxel Size read-only with its dimensions (they are the asset's), the
// component's fields through the Catalog (PropertyCmd), so undo, multi-selection and prefab overrides
// work as on any component; Make Unique, Apply to Voxel Size, Regenerate and Re-seed From Terrain.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "component/VoxelVolumeComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Enums.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelMaterials.h"
#include "voxel/VoxelVolumeOps.h"
#include "window/VoxelEditWindow.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

void editor::InspectorWindow::drawVoxelVolumeComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    const bool single = entities.size() == 1;
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    const std::shared_ptr<VoxelAsset> asset = system ? system->getAsset(entity) : nullptr;
    static std::string actionError;

    if (single) {
        drawVoxelProblemStrip(*project, *sceneProject, entity);
        if (ImGui::Button(ICON_FA_CUBES " Edit Volume", ImVec2(-1, 0))) {
            if (VoxelEditWindow* window = Backend::getApp().getVoxelEditWindow()) window->openForEntity(entity, sceneProject->id);
        }
    }

    std::vector<EnumEntry>* quality = InspectorWindow::enumEntries(reflect::enums::kVoxelQuality);
    std::vector<EnumEntry>* navigation = InspectorWindow::enumEntries(reflect::enums::kVoxelNavigationUpdate);
    std::vector<EnumEntry>* terrainCut = InspectorWindow::enumEntries(reflect::enums::kVoxelTerrainCut);
    static std::vector<int> lodRadii = {4, 6, 8};

    beginTable(cpType, getLabelSize("Navigation Update"));
    // Voxel Size belongs to the file (R1): shown here, changed with Apply to Voxel Size
    if (propertyHeader("Voxel Size")) {
        ImGui::BeginDisabled();
        float voxelSize = asset && asset->store ? asset->store->desc().voxelSize : 0.0f;
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##voxelSize", &voxelSize, 0.0f, 0.0f, 0.0f, "%.3f m");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The asset's: scale the Transform and use Apply to Voxel Size to change it");
    }
    if (asset && asset->store && propertyHeader("Dimensions")) {
        const voxel::Int3 size = asset->store->desc().dimensions;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d x %d x %d", size.x, size.y, size.z);
    }
    for (const VoxelInspectorRow& row : voxelInspectorRows()) {
        RowSettings settings;
        settings.help = row.help;
        const std::string property = row.property;
        switch (row.kind) {
            case VoxelInspectorRowKind::Bool:
                propertyRow(RowPropertyType::Bool, cpType, property, row.label, sceneProject, entities, settings);
                break;
            case VoxelInspectorRowKind::Int:
                settings.intMin = 0;
                settings.intMax = property == "lodLevels" ? 4 : (property == "lodRadius" ? 8 : 0);
                if (property == "lodRadius") {
                    settings.intMin = 4;
                    settings.sliderValues = &lodRadii;
                    propertyRow(RowPropertyType::IntSlider, cpType, property, row.label, sceneProject, entities, settings);
                } else {
                    propertyRow(RowPropertyType::Int, cpType, property, row.label, sceneProject, entities, settings);
                }
                break;
            case VoxelInspectorRowKind::Float:
                propertyRow(RowPropertyType::Float, cpType, property, row.label, sceneProject, entities, settings);
                break;
            case VoxelInspectorRowKind::Enum:
                settings.enumEntries = property == "quality" ? quality : (property == "navigationUpdate" ? navigation : terrainCut);
                propertyRow(RowPropertyType::Enum, cpType, property, row.label, sceneProject, entities, settings);
                break;
            case VoxelInspectorRowKind::Entity:
                propertyRow(RowPropertyType::LocalEntity, cpType, property, row.label, sceneProject, entities, settings);
                break;
            case VoxelInspectorRowKind::Asset:
                propertyRow(RowPropertyType::Asset, cpType, property, row.label, sceneProject, entities, settings);
                break;
        }
    }
    endTable();

    if (!single) return;

    // R1: a scale the volume ignores
    if (const Transform* transform = scene->findComponent<Transform>(entity)) {
        const Vector3 scale = transform->scale;
        if (std::fabs(scale.x - 1.0f) > 1e-4f || std::fabs(scale.y - 1.0f) > 1e-4f || std::fabs(scale.z - 1.0f) > 1e-4f) {
            if (ImGui::Button("Apply to Voxel Size")) {
                actionError.clear();
                applyScaleToVoxelSize(project, sceneProject->id, entity, &actionError);
            }
            ImGui::SetItemTooltip("Moves a uniform Scale into the asset's Voxel Size and sets the Scale back to 1");
        }
    }

    // R65: Duplicate shares the asset
    if (asset && asset->store) {
        const int users = voxelAssetUserCount(project, asset->path);
        if (ImGui::Button(ICON_FA_CLONE " Make Unique")) {
            actionError.clear();
            makeVoxelVolumeUnique(project, sceneProject->id, entity, &actionError);
        }
        ImGui::SetItemTooltip("Give this GameObject its own copy of the volume, with a new GUID");
        if (users > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d GameObjects share this volume", users);
        }
    }

    VoxelEditWindow* window = Backend::getApp().getVoxelEditWindow();
    ImGui::BeginDisabled(!asset || !asset->store || !window || window->isGenerating());
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Regenerate") && window) window->startRegenerate(sceneProject->id, entity);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Build the base again from its source. Modified blocks stay.");
    ImGui::SameLine();
    const bool terrainBase = asset && (asset->generator.kind == "terrain" || asset->generator.otherKind == "terrain");
    ImGui::BeginDisabled(!terrainBase);
    if (ImGui::Button(ICON_FA_MOUNTAIN_SUN " Re-seed From Terrain")) {
        actionError.clear();
        reseedVoxelVolumeFromTerrain(project, sceneProject->id, entity, nullptr, &actionError);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Discard modified blocks and take the terrain again. One undo step.");

    if (!actionError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", actionError.c_str());
        ImGui::PopTextWrapPos();
    }
}
