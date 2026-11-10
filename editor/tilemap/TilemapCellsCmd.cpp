// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapCellsCmd.h"

#include "Project.h"
#include "component/TilemapLayerComponent.h"

using namespace jasperengine;

editor::tilemap::TilemapCellsCmd::TilemapCellsCmd(Project* project, uint32_t sceneId, std::vector<LayerChanges> layers, std::string name):
    project(project), sceneId(sceneId), layerChanges(std::move(layers)), name(std::move(name)) {
}

editor::tilemap::TilemapCellsCmd::TilemapCellsCmd(Project* project, uint32_t sceneId, Entity layer, std::vector<TileCellChange> changes, std::string name):
    project(project), sceneId(sceneId), name(std::move(name)) {
    LayerChanges changesOfLayer;
    changesOfLayer.layer = layer;
    changesOfLayer.changes = std::move(changes);
    layerChanges.push_back(std::move(changesOfLayer));
}

bool editor::tilemap::TilemapCellsCmd::apply(bool forward) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return false;
    bool applied = false;
    for (const LayerChanges& layer : layerChanges) {
        TilemapLayerComponent* component = sceneProject->scene->findComponent<TilemapLayerComponent>(layer.layer);
        if (!component) continue;
        TileCells& cells = component->cells.edit();
        for (const TileCellChange& change : layer.changes) {
            cells.set(change.cell, forward ? change.newValue : change.oldValue);
            cells.setTerrainValue(change.cell, forward ? change.newTerrain : change.oldTerrain);
        }
        applied = true;
    }
    if (applied) {
        sceneProject->isModified = true;
        sceneProject->needUpdateRender = true;
    }
    return applied;
}

bool editor::tilemap::TilemapCellsCmd::execute() {
    if (isNoOp()) return true;
    return apply(true);
}

void editor::tilemap::TilemapCellsCmd::undo() {
    apply(false);
}

bool editor::tilemap::TilemapCellsCmd::isNoOp() const {
    for (const LayerChanges& layer : layerChanges) {
        if (!layer.changes.empty()) return false;
    }
    return true;
}

size_t editor::tilemap::TilemapCellsCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + name.capacity() + layerChanges.capacity() * sizeof(LayerChanges);
    for (const LayerChanges& layer : layerChanges) bytes += layer.changes.capacity() * sizeof(TileCellChange);
    return bytes;
}
