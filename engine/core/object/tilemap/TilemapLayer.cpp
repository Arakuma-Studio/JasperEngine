// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "object/tilemap/TilemapLayer.h"

#include "component/GridComponent.h"
#include "component/Transform.h"
#include "math/Matrix4.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TerrainSolver.h"

#include <vector>

using namespace jasperengine;

TilesetHandle::TilesetHandle(std::shared_ptr<const TilesetRuntime> runtime): runtime(std::move(runtime)){
}

int32_t TilesetHandle::findTile(const std::string& name) const{
    return runtime ? runtime->data->findTileByName(name) : NO_TILE;
}

std::string TilesetHandle::getTileName(int32_t tile) const{
    if (!runtime)
        return std::string();
    const TileDef* def = runtime->data->findTile(tile);
    return def ? runtime->data->tileName(*def) : std::string();
}

int TilesetHandle::getTileCount() const{
    return runtime ? (int)runtime->data->tiles.size() : 0;
}

int TilesetHandle::getTerrainCount() const{
    return runtime ? runtime->data->terrainCount() : 0;
}

int TilesetHandle::findTerrain(const std::string& name) const{
    return runtime ? runtime->data->findTerrain(name) : NO_TERRAIN;
}

std::string TilesetHandle::getTerrainName(int terrain) const{
    if (!runtime)
        return std::string();
    const TerrainDef* def = runtime->data->terrain(terrain);
    return def ? def->name : std::string();
}

const TilePropertyValue* TilesetHandle::findProperty(int32_t tile, const std::string& name) const{
    if (!runtime)
        return nullptr;
    const TileDef* def = runtime->data->findTile(tile);
    if (!def)
        return nullptr;
    auto found = def->properties.find(name);
    if (found != def->properties.end())
        return &found->second;
    const PropertySchemaEntry* schema = runtime->data->findProperty(name);
    return schema ? &schema->defaultValue : nullptr;
}

int TilesetHandle::getNavArea(int32_t tile) const{
    if (!runtime)
        return -1;
    const TileDef* def = runtime->data->findTile(tile);
    return def ? def->navArea : -1;
}

TilemapLayer::TilemapLayer(Scene* scene): Mesh(scene){
    addComponent<TilemapLayerComponent>();
}

TilemapLayer::TilemapLayer(Scene* scene, Entity entity): Mesh(scene, entity){
}

TilemapLayer::~TilemapLayer(){
}

TilemapLayer::TilemapLayer(const TilemapLayer& rhs): Mesh(rhs){
}

TilemapLayer& TilemapLayer::operator=(const TilemapLayer& rhs){
    Mesh::operator=(rhs);
    return *this;
}

TileCells& TilemapLayer::editCells(){
    TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    TileCells& cells = layer.cells.edit();
    if (cells.getChunkSize() != layer.chunkSize && TileCells::isValidChunkSize(layer.chunkSize))
        cells.setChunkSize(layer.chunkSize);
    return cells;
}

const TileCells& TilemapLayer::readCells() const{
    return getComponent<TilemapLayerComponent>().cells.read();
}

TileGridSpec TilemapLayer::gridSpec() const{
    return scene->getSystem<TilemapSystem>()->layerSpec(entity, getComponent<TilemapLayerComponent>());
}

TilesetHandle TilemapLayer::getTileset() const{
    const TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    return TilesetHandle(layer.tileset.empty() ? nullptr : TilesetPool::get(layer.tileset.path));
}

void TilemapLayer::setCell(int x, int y, int tile, uint32_t flags){
    editCells().setTile(CellCoord{x, y}, tile, flags);
}

void TilemapLayer::eraseCell(int x, int y){
    setCell(x, y, NO_TILE);
}

int TilemapLayer::getTile(int x, int y, uint32_t* flags) const{
    return readCells().getTile(CellCoord{x, y}, flags);
}

void TilemapLayer::fill(int x0, int y0, int x1, int y1, int tile, uint32_t flags){
    const CellRect rect = CellRect::of(CellCoord{x0, y0}, CellCoord{x1, y1});
    if (rect.area() > MAX_TILE_REGION_CELLS)
        return;
    TileCells& cells = editCells();
    for (int64_t y = rect.minY; y <= rect.maxY; y++){
        for (int64_t x = rect.minX; x <= rect.maxX; x++)
            cells.setTile(CellCoord{(int32_t)x, (int32_t)y}, tile, flags);
    }
}

void TilemapLayer::clear(){
    TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    if (layer.cells.isSet())
        layer.cells.edit().clear();
}

CellRect TilemapLayer::getUsedRect() const{
    return readCells().usedRect();
}

int TilemapLayer::getCellCount() const{
    return (int)readCells().cellCount();
}

int TilemapLayer::getChunkCount() const{
    return (int)readCells().chunkCount();
}

CellCoord TilemapLayer::worldToCell(const Vector3& world) const{
    const TileGridSpec spec = gridSpec();
    const Vector3 local = getComponent<Transform>().modelMatrix.inverse() * world;
    const float xyz[3] = {local.x, local.y, local.z};
    return tilegrid::localToCell(spec, tilegrid::localToPlane(spec, xyz));
}

Vector3 TilemapLayer::cellToWorld(int x, int y) const{
    const TileGridSpec spec = gridSpec();
    float local[3];
    tilegrid::planeToLocal(spec, tilegrid::cellToLocal(spec, CellCoord{x, y}), local);
    return getComponent<Transform>().modelMatrix * Vector3(local[0], local[1], local[2]);
}

bool TilemapLayer::getCellAt(const Vector3& world, int& tile, CellCoord& cell) const{
    const TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    const TileGridSpec spec = gridSpec();
    const Vector3 local = getComponent<Transform>().modelMatrix.inverse() * world;
    const float xyz[3] = {local.x, local.y, local.z};
    TilePoint point = tilegrid::localToPlane(spec, xyz);
    if (layer.dualGrid){
        // the shown tiles sit half a cell down-left of the logical cells (R27)
        point.x += tilegrid::pitchX(spec) * 0.5;
        point.y += tilegrid::pitchY(spec) * 0.5;
    }
    cell = tilegrid::localToCell(spec, point);
    tile = layer.cells.read().getTile(cell);
    return tile != NO_TILE;
}

namespace{

    bool makeTerrainContext(const TilemapLayer& layer, const TilemapLayerComponent& component, TerrainContext& context, TilesetHandle& keepAlive, std::string& error){
        keepAlive = layer.getTileset();
        if (!keepAlive.isValid()){
            error = "the layer has no Tileset (assign one to its tileset field)";
            return false;
        }
        context.tileset = keepAlive.data();
        context.grid = layer.gridSpec();
        context.dualGrid = component.dualGrid;
        context.seed = component.seed;
        return true;
    }

}

bool TilemapLayer::paintTerrain(const CellRect& rect, int terrainIndex, std::string& error){
    TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    if (rect.area() > MAX_TILE_REGION_CELLS){
        error = "the rectangle has more than 1,000,000 cells";
        return false;
    }
    TerrainContext context;
    TilesetHandle keep;
    if (!makeTerrainContext(*this, layer, context, keep, error))
        return false;
    std::vector<CellCoord> cells;
    cells.reserve((size_t)rect.area());
    for (int64_t y = rect.minY; y <= rect.maxY; y++){
        for (int64_t x = rect.minX; x <= rect.maxX; x++)
            cells.push_back(CellCoord{(int32_t)x, (int32_t)y});
    }
    TileCellPatch patch;
    TerrainReport report;
    return terrain::paint(editCells(), context, cells, terrainIndex, patch, report, error);
}

int TilemapLayer::getTerrain(int x, int y) const{
    return readCells().getTerrain(CellCoord{x, y});
}

bool TilemapLayer::refreshAutotile(const CellRect& rect, std::string& error){
    TilemapLayerComponent& layer = getComponent<TilemapLayerComponent>();
    TerrainContext context;
    TilesetHandle keep;
    if (!makeTerrainContext(*this, layer, context, keep, error))
        return false;
    TileCellPatch patch;
    TerrainReport report;
    terrain::refresh(editCells(), context, rect, patch, report);
    return true;
}
