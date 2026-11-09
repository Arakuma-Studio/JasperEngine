// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A Tilemap Layer in Lua and C++ scripts (docs/specs/S50-grid-tilemap.md R29, R80, §6.4). Edits
// write the cells; the TilemapSystem rebuilds the touched chunks once per frame, whatever the
// number of edits. Edits in Play are not saved. Main thread only (ECS).

#ifndef OBJECT_TILEMAP_LAYER_H
#define OBJECT_TILEMAP_LAYER_H

#include "object/Mesh.h"
#include "component/TilemapLayerComponent.h"
#include "math/Rect.h"
#include "tilemap/TileGridLayout.h"
#include "tilemap/TilesetPool.h"

#include <memory>
#include <string>

namespace jasperengine{

    // Lua `Tileset`: the lookups of the layer's tileset
    class JASPERENGINE_API TilesetHandle{
    public:
        TilesetHandle() = default;
        explicit TilesetHandle(std::shared_ptr<const TilesetRuntime> runtime);

        bool isValid() const { return (bool)runtime; }
        int32_t findTile(const std::string& name) const;
        std::string getTileName(int32_t tile) const;
        int getTileCount() const;
        int getTerrainCount() const;
        int findTerrain(const std::string& name) const;
        std::string getTerrainName(int terrain) const;
        const TilePropertyValue* findProperty(int32_t tile, const std::string& name) const;
        int getNavArea(int32_t tile) const;
        const TilesetData* data() const { return runtime ? runtime->data.get() : nullptr; }

    private:
        std::shared_ptr<const TilesetRuntime> runtime;
    };

    class JASPERENGINE_API TilemapLayer: public Mesh{
    public:
        explicit TilemapLayer(Scene* scene);
        TilemapLayer(Scene* scene, Entity entity);
        virtual ~TilemapLayer();

        TilemapLayer(const TilemapLayer& rhs);
        TilemapLayer& operator=(const TilemapLayer& rhs);

        TileCells& editCells();
        const TileCells& readCells() const;
        TileGridSpec gridSpec() const;
        TilesetHandle getTileset() const;

        void setCell(int x, int y, int tile, uint32_t flags = 0);
        void eraseCell(int x, int y);
        int getTile(int x, int y, uint32_t* flags = nullptr) const;
        void fill(int x0, int y0, int x1, int y1, int tile, uint32_t flags = 0);
        void clear();

        CellRect getUsedRect() const;
        int getCellCount() const;
        int getChunkCount() const;

        // the cell a world point falls in: logical on a dual-grid layer
        CellCoord worldToCell(const Vector3& world) const;
        // a cell's anchor in world space
        Vector3 cellToWorld(int x, int y) const;
        // the shown tile at a world point: false outside any tile
        bool getCellAt(const Vector3& world, int& tile, CellCoord& cell) const;

        // terrain (R23-R25): "" and false with `error` on a bad name or mode; the tile cells around
        // re-chosen by the solver
        bool paintTerrain(const CellRect& rect, int terrainIndex, std::string& error);
        int getTerrain(int x, int y) const;
        bool refreshAutotile(const CellRect& rect, std::string& error);
    };

}

#endif //OBJECT_TILEMAP_LAYER_H
