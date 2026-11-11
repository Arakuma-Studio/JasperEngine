// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "object/tilemap/Grid.h"

#include "component/Transform.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"

using namespace jasperengine;

Grid::Grid(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<GridComponent>(entity))
        scene->addComponent<GridComponent>(entity, GridComponent());
}

Grid::~Grid(){
}

Grid::Grid(const Grid& rhs): EntityHandle(rhs){
}

Grid& Grid::operator=(const Grid& rhs){
    EntityHandle::operator=(rhs);
    return *this;
}

void Grid::setLayout(TileGridLayout layout){
    GridComponent& grid = getComponent<GridComponent>();
    grid.layout = layout;
    grid.revision++;
}

TileGridLayout Grid::getLayout() const{
    return getComponent<GridComponent>().layout;
}

void Grid::setCellSize(Vector2 size){
    GridComponent& grid = getComponent<GridComponent>();
    grid.cellSize = size;
    grid.revision++;
}

Vector2 Grid::getCellSize() const{
    return getComponent<GridComponent>().cellSize;
}

void Grid::setCellGap(Vector2 gap){
    GridComponent& grid = getComponent<GridComponent>();
    grid.cellGap = gap;
    grid.revision++;
}

Vector2 Grid::getCellGap() const{
    return getComponent<GridComponent>().cellGap;
}

CellCoord Grid::worldToCell(const Vector3& world) const{
    const Transform& transform = getComponent<Transform>();
    const TileGridSpec spec = gridSpecOf(getComponent<GridComponent>());
    const Vector3 local = transform.modelMatrix.inverse() * world;
    const float xyz[3] = {local.x, local.y, local.z};
    return tilegrid::localToCell(spec, tilegrid::localToPlane(spec, xyz));
}

Vector3 Grid::cellToWorld(int x, int y) const{
    const Transform& transform = getComponent<Transform>();
    const TileGridSpec spec = gridSpecOf(getComponent<GridComponent>());
    float local[3];
    tilegrid::planeToLocal(spec, tilegrid::cellToLocal(spec, CellCoord{x, y}), local);
    return transform.modelMatrix * Vector3(local[0], local[1], local[2]);
}
