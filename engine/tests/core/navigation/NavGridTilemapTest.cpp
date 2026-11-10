// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// AC14 (docs/specs/S50-grid-tilemap.md R51): a NavGrid whose source is a Tilemap Layer takes each
// cell's area from the tile at its centre: the tile's nav_area wins, else S19's tile-name table,
// else walkable; an empty cell takes emptyCellArea. The tile at a point is the drawn one: on a
// dual-grid layer, half a cell down-left (R27).

#include "NavTestUtil.h"

#include "component/GridComponent.h"
#include "component/NavGridComponent.h"
#include "component/TilemapLayerComponent.h"
#include "object/navigation/NavGrid.h"
#include "object/tilemap/TilemapLayer.h"
#include "tilemap/TilesetPool.h"

#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::navtest;
using nav::GridCell;

namespace {

    const char* const NAV_TILESET = "nav-grid-tilemap.tileset";

    // tile 0 "floor" (no nav_area, in the table), 1 "water" (nav_area 3, the table says 5),
    // 2 "rock" (no nav_area, not in the table)
    std::shared_ptr<TilesetData> navTileset() {
        auto data = std::make_shared<TilesetData>();
        data->tileWidth = 32;
        data->tileHeight = 32;
        TilesetSource source;
        source.id = 0;
        data->sources.push_back(source);
        const char* names[] = {"floor", "water", "rock"};
        for (int i = 0; i < 3; i++) {
            TileDef& tile = data->addTile(0, i, 0);
            tile.name = names[i];
        }
        data->findTile(1)->navArea = 3;
        return data;
    }

    class NavGridTilemapTest : public NavSceneTest {
    protected:
        std::unique_ptr<TilemapLayer> layer;
        Entity grid = NULL_ENTITY;

        void SetUp() override {
            NavSceneTest::SetUp();
            TilesetPool::set(NAV_TILESET, navTileset());
            layer = std::make_unique<TilemapLayer>(scene.get());
            GridComponent cells;
            cells.cellSize = Vector2(32.0f, 32.0f);
            scene->addComponent<GridComponent>(layer->getEntity(), cells);
            layer->getComponent<TilemapLayerComponent>().tileset = AssetRef(NAV_TILESET);

            grid = transformEntity(*scene, Vector3(), NULL_ENTITY, "NavGrid");
            NavGridComponent component;
            component.width = 4;
            component.height = 2;
            component.cellSize = 32.0f;
            component.plane = nav::NavPlane::XY;
            component.source = NavGridSource::TILEMAP;
            component.sourceEntity = layer->getEntity();
            component.tileAreas = {{"floor", 4}, {"water", 5}};
            component.emptyCellArea = nav::AREA_NOT_WALKABLE;
            scene->addComponent<NavGridComponent>(grid, component);
        }

        void TearDown() override {
            layer.reset();
            TilesetPool::invalidate(NAV_TILESET);
            NavSceneTest::TearDown();
        }
    };

}

TEST_F(NavGridTilemapTest, TileAreaThenNameTableThenEmptyCellArea) {
    // row 0: floor, water, rock, empty; row 1: empty, floor, empty, water
    layer->setCell(0, 0, 0);
    layer->setCell(1, 0, 1);
    layer->setCell(2, 0, 2);
    layer->setCell(1, 1, 0);
    layer->setCell(3, 1, 1);
    resolveTransforms(*scene);
    NavGrid navGrid(scene.get(), grid);
    ASSERT_TRUE(navGrid.rebuild());
    EXPECT_EQ(navGrid.getCellArea(GridCell{0, 0}), 4) << "the name table";
    EXPECT_EQ(navGrid.getCellArea(GridCell{1, 0}), 3) << "the tile's nav_area wins over the table";
    EXPECT_EQ(navGrid.getCellArea(GridCell{2, 0}), nav::AREA_WALKABLE) << "a tile without either";
    EXPECT_EQ(navGrid.getCellArea(GridCell{3, 0}), nav::AREA_NOT_WALKABLE) << "emptyCellArea";
    EXPECT_EQ(navGrid.getCellArea(GridCell{0, 1}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(navGrid.getCellArea(GridCell{1, 1}), 4);
    EXPECT_EQ(navGrid.getCellArea(GridCell{3, 1}), 3);

    // emptyCellArea is what an empty cell takes, whatever it is
    scene->getComponent<NavGridComponent>(grid).emptyCellArea = 2;
    ASSERT_TRUE(navGrid.rebuild());
    EXPECT_EQ(navGrid.getCellArea(GridCell{3, 0}), 2);
    EXPECT_EQ(navGrid.getCellArea(GridCell{0, 0}), 4);
}

// the drawn tile under a NavGrid cell's centre: a dual-grid tile (x, y) covers the corner of the
// logical cells (x - 1, y - 1)..(x, y), so the centre of cell (x, y) shows tile (x + 1, y + 1)
TEST_F(NavGridTilemapTest, DualGridLayersGiveTheDrawnTile) {
    layer->getComponent<TilemapLayerComponent>().dualGrid = true;
    layer->setCell(1, 1, 1);
    layer->setCell(3, 2, 0);
    resolveTransforms(*scene);
    NavGrid navGrid(scene.get(), grid);
    ASSERT_TRUE(navGrid.rebuild());
    EXPECT_EQ(navGrid.getCellArea(GridCell{0, 0}), 3);
    EXPECT_EQ(navGrid.getCellArea(GridCell{1, 0}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(navGrid.getCellArea(GridCell{1, 1}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(navGrid.getCellArea(GridCell{2, 1}), 4);
}

// the layer's tileAnchor moves its tiles, and the NavGrid reads the tile drawn over each cell's
// centre: an anchor of one cell right shows tile (x - 1, y) there (tilegrid::tileShift, R8)
TEST_F(NavGridTilemapTest, TileAnchorGivesTheDrawnTile) {
    layer->getComponent<TilemapLayerComponent>().tileAnchor = Vector2(1.0f, 0.0f);
    layer->setCell(0, 0, 1);
    layer->setCell(2, 1, 0);
    resolveTransforms(*scene);
    NavGrid navGrid(scene.get(), grid);
    ASSERT_TRUE(navGrid.rebuild());
    EXPECT_EQ(navGrid.getCellArea(GridCell{0, 0}), nav::AREA_NOT_WALKABLE) << "the logical cell (0, 0) draws one cell right";
    EXPECT_EQ(navGrid.getCellArea(GridCell{1, 0}), 3);
    EXPECT_EQ(navGrid.getCellArea(GridCell{2, 1}), nav::AREA_NOT_WALKABLE);
    EXPECT_EQ(navGrid.getCellArea(GridCell{3, 1}), 4);
}
