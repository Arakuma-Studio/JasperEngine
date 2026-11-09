// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting and Sprite Batch in the Inspector (docs/specs/S52-sprite-instancing.md R31-R33): the
// renderers' Sorting Layer and Order in Layer rows, the Sorting Group and the Sprite Batch. Every
// edit is one scene undo step (PropertyCmd through propertyRow).

#include "InspectorWindow.h"

#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "reflect/Components.h"
#include "registry/SortingLayers.h"
#include "util/UIProbe.h"

#include "imgui.h"

using namespace jasperengine;

void editor::InspectorWindow::drawSortingRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    // the project's layers in draw order (names first: the entries point into them)
    const std::vector<SortingLayerInfo>& layers = SortingLayers::getLayers();
    sortingLayerNames.clear();
    for (const SortingLayerInfo& layer : layers)
        sortingLayerNames.push_back(layer.name);
    sortingLayerEntries.clear();
    for (size_t i = 0; i < layers.size(); i++)
        sortingLayerEntries.push_back({layers[i].id, sortingLayerNames[i].c_str()});

    RowSettings layerSettings;
    layerSettings.enumEntries = &sortingLayerEntries;
    layerSettings.enumExtraLabel = "Add Sorting Layer...";
    layerSettings.enumExtraAction = [this](){ tagsAndLayersRequested = true; };
    layerSettings.help = "Draws before the layers after it in Project Settings > Tags and Layers > Sorting Layers.";
    propertyRow(RowPropertyType::Enum, cpType, "sortingLayer", "Sorting Layer", sceneProject, entities, layerSettings);
    UIProbe::item("sorting.sorting_layer");

    RowSettings orderSettings;
    orderSettings.intMin = -32768;
    orderSettings.intMax = 32767;
    orderSettings.help = "Lower draws first within its Sorting Layer.";
    propertyRow(RowPropertyType::Int, cpType, "sortingOrder", "Order in Layer", sceneProject, entities, orderSettings);
    UIProbe::item("sorting.order_in_layer");
}

void editor::InspectorWindow::drawSortingGroupComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    beginTable(cpType, getLabelSize("Order in Layer"));
    drawSortingRows(cpType, sceneProject, entities);
    endTable();
}

void editor::InspectorWindow::drawSpriteBatchComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    Scene* scene = sceneProject->scene;

    RowSettings modeSettings;
    modeSettings.enumEntries = enumEntries(reflect::enums::kSpriteBatchMode);
    modeSettings.help = "Retained: the instances stay until scripts change them. Clear Each Frame: what scripts add in a frame is drawn once.";
    RowSettings spaceSettings;
    spaceSettings.enumEntries = enumEntries(reflect::enums::kSpriteBatchSpace);
    spaceSettings.help = "Local: the instances move with this GameObject. World: their positions are world positions.";
    RowSettings capacitySettings;
    capacitySettings.help = "Instances the GPU buffer holds before it grows (the bulk calls grow it).";
    RowSettings sortSettings;
    sortSettings.help = "Draws the instances along Project Settings > Graphics > Transparency Sort Axis, else in their index order.";

    beginTable(cpType, getLabelSize("Sort Instances"));
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, modeSettings);
    propertyRow(RowPropertyType::Enum, cpType, "space", "Space", sceneProject, entities, spaceSettings);
    propertyRow(RowPropertyType::UInt, cpType, "capacity", "Capacity", sceneProject, entities, capacitySettings);
    propertyRow(RowPropertyType::Bool, cpType, "sortInstances", "Sort Instances", sceneProject, entities, sortSettings);
    endTable();

    // the instances are runtime data: their count, read only
    const SpriteBatchComponent& batch = scene->getComponent<SpriteBatchComponent>(entities[0]);
    ImGui::BeginDisabled();
    ImGui::Text("Instances: %u", batch.count);
    ImGui::EndDisabled();
    UIProbe::item("spritebatch.instances");
    if (batch.count == 0){
        ImGui::TextDisabled("Filled by scripts: SpriteBatch:setPositions (docs/rendering/sprite-batch.md)");
    }
}
