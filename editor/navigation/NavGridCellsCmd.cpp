// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavGridCellsCmd.h"

#include "Project.h"
#include "component/NavGridComponent.h"
#include "subsystem/NavigationSystem.h"

using namespace jasperengine;

editor::NavGridCellsCmd::NavGridCellsCmd(Project* project, uint32_t sceneId, Entity grid, std::vector<Cell> cells, std::string name):
    project(project), sceneId(sceneId), grid(grid), cells(std::move(cells)), name(std::move(name)) {
}

editor::NavGridCellsCmd::NavGridCellsCmd(Project* project, uint32_t sceneId, Entity grid, std::vector<uint8_t> before,
                                         std::vector<uint8_t> after, std::string name):
    project(project), sceneId(sceneId), grid(grid), before(std::move(before)), after(std::move(after)), name(std::move(name)) {
}

bool editor::NavGridCellsCmd::apply(bool forward) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return false;
    auto system = sceneProject->scene->getSystem<NavigationSystem>();
    NavGridComponent* component = sceneProject->scene->findComponent<NavGridComponent>(grid);
    nav::NavGridMap* map = system && component ? system->getGridMap(grid) : nullptr;
    if (!map) return false;
    if (!before.empty() || !after.empty()) {
        const std::vector<uint8_t>& bytes = forward ? after : before;
        std::string error;
        nav::NavGridMap restored;
        if (!restored.read(bytes.data(), bytes.size(), error)) return false;
        // the map object stays (agents and overlays hold it); its cells change
        for (int i = 0; i < std::min(map->getCellCount(), restored.getCellCount()); i++) {
            const nav::GridCell cell = map->cellOf(i);
            map->setArea(cell, restored.getAreaAt(i));
            map->setOverride(cell, restored.getOverride(cell));
            if (restored.hasHeights()) {
                map->setHasHeights(true);
                map->setHeight(cell, restored.getHeight(cell));
            }
        }
        if (!restored.hasHeights() && map->hasHeights()) map->setHasHeights(false);
    } else {
        for (const Cell& cell : cells) {
            if (cell.index < 0 || cell.index >= map->getCellCount()) continue;
            const nav::GridCell c = map->cellOf(cell.index);
            map->setArea(c, forward ? cell.newArea : cell.oldArea);
            map->setOverride(c, forward ? cell.newOverride : cell.oldOverride);
        }
    }
    component->mapDirty = true;
    sceneProject->isModified = true;
    sceneProject->needUpdateRender = true;
    return true;
}

bool editor::NavGridCellsCmd::execute() {
    return apply(true);
}

void editor::NavGridCellsCmd::undo() {
    apply(false);
}
