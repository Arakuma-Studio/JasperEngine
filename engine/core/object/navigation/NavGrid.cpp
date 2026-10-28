// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavGrid.h"

#include "Log.h"
#include "Scene.h"
#include "subsystem/NavigationSystem.h"

using namespace jasperengine;

NavGrid::NavGrid(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

NavGrid::~NavGrid() = default;
NavGrid::NavGrid(const NavGrid& rhs) = default;
NavGrid& NavGrid::operator=(const NavGrid& rhs) = default;
NavGrid::NavGrid(NavGrid&& rhs) noexcept = default;
NavGrid& NavGrid::operator=(NavGrid&& rhs) noexcept = default;

NavGridComponent& NavGrid::component() const{
    return getComponent<NavGridComponent>();
}

NavigationSystem* NavGrid::system() const{
    return scene->getSystem<NavigationSystem>().get();
}

bool NavGrid::isValid() const{
    return scene && scene->findComponent<NavGridComponent>(entity) != nullptr;
}

nav::GridPath NavGrid::findPath(nav::GridCell from, nav::GridCell to, const nav::GridQuery& query) const{
    nav::GridQuery q = query;
    q.movement = component().movement;
    return system()->findGridPath(entity, from, to, q);
}

std::vector<Vector3> NavGrid::getPathPoints(const nav::GridPath& path) const{
    return system()->gridPathToWorld(entity, path);
}

nav::GridCell NavGrid::worldToCell(const Vector3& world) const{ return system()->gridWorldToCell(entity, world); }
Vector3 NavGrid::cellToWorld(nav::GridCell cell) const{ return system()->gridCellToWorld(entity, cell); }
bool NavGrid::setCellArea(nav::GridCell cell, int area){ return area >= 0 && area < nav::AREA_COUNT && system()->setGridCellArea(entity, cell, (uint8_t)area); }

int NavGrid::getCellArea(nav::GridCell cell) const{
    nav::NavGridMap* map = system()->getGridMap(entity);
    return map && map->contains(cell) ? map->getArea(cell) : -1;
}

int NavGrid::fillRect(nav::GridCell from, nav::GridCell to, int area){
    if (area < 0 || area >= nav::AREA_COUNT)
        return 0;
    return system()->fillGridRect(entity, from, to, (uint8_t)area);
}

int NavGrid::stampObstacle(const AABB& worldBounds){ return system()->stampGridObstacle(entity, worldBounds); }

bool NavGrid::isReachable(nav::GridCell from, nav::GridCell to) const{
    nav::NavGridMap* map = system()->getGridMap(entity);
    return map && map->isReachable(from, to);
}

bool NavGrid::contains(nav::GridCell cell) const{
    nav::NavGridMap* map = system()->getGridMap(entity);
    return map && map->contains(cell);
}

bool NavGrid::rebuild(){
    std::string error;
    if (!system()->rebuildGrid(entity, error)){
        Log::error("NavGrid.rebuild: %s", error.c_str());
        return false;
    }
    return true;
}

void NavGrid::setEnabled(bool enabled){ component().enabled = enabled; }
bool NavGrid::isEnabled() const{ return component().enabled; }
int NavGrid::getWidth() const{ return component().width; }
int NavGrid::getHeight() const{ return component().height; }
float NavGrid::getCellSize() const{ return component().cellSize; }
nav::GridLayout NavGrid::getLayout() const{ return component().layout; }
