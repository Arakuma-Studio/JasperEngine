// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Where a layer's tile for a cell is (docs/specs/S50-grid-tilemap.md R8, R10, R27, R2 tileAnchor):
// the drawn instance (TilemapChunkBuilder), the collider (TileColliderBuilder) and the cell a world
// point names (TilemapLayer::getCellAt, which picking and the NavGrid share through
// tilegrid::shownCellAt) agree, on rectangles and isometric diamonds, with a dual grid and a
// tileAnchor. The expected positions are written out here from R7/R8/R27.

#include "Engine.h"
#include "Scene.h"
#include "component/GridComponent.h"
#include "component/TilemapLayerComponent.h"
#include "object/tilemap/TilemapLayer.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "tilemap/TileColliderBuilder.h"
#include "tilemap/TilemapChunkBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

using namespace jasperengine;

namespace {

    struct PlacementCase {
        const char* name;
        TileGridLayout layout;
        double anchorX, anchorY;
        bool dualGrid;
        // the drawn quad's bottom-left for cell (3, 2), pixels, written from R7/R8/R27
        double x0, y0;
    };

    // rectangle cells 16 x 16: cell (3, 2) is anchored at (48, 32); isometric diamonds 32 x 16:
    // at ((3 - 2) * 16, (3 + 2) * 8) = (16, 40), the image bottom-centred there (x0 = 0).
    // tileAnchor (ax, ay) moves by (ax * 16|32, ay * 16); a dual grid by (-8, -8) on rectangles,
    // (0, -8) on diamonds
    const PlacementCase kPlacementCases[] = {
        {"rectangle", TileGridLayout::Rectangle, 0.0, 0.0, false, 48.0, 32.0},
        {"rectangle dual grid + anchor", TileGridLayout::Rectangle, 0.25, 0.5, true, 44.0, 32.0},
        {"isometric", TileGridLayout::Isometric, 0.0, 0.0, false, 0.0, 40.0},
        {"isometric dual grid", TileGridLayout::Isometric, 0.0, 0.0, true, 0.0, 32.0},
        {"isometric anchor", TileGridLayout::Isometric, 0.5, 0.25, false, 16.0, 44.0},
        {"isometric dual grid + anchor", TileGridLayout::Isometric, 0.5, 0.25, true, 16.0, 36.0},
    };

    constexpr CellCoord kPlacedCell{3, 2};

    TileGridSpec placementGrid(TileGridLayout layout) {
        TileGridSpec spec;
        spec.layout = layout;
        spec.cellWidth = layout == TileGridLayout::Rectangle ? 16.0 : 32.0;
        spec.cellHeight = 16.0;
        return spec;
    }

    std::shared_ptr<TilesetRuntime> placementTileset(const TileGridSpec& spec) {
        auto data = std::make_shared<TilesetData>();
        data->tileWidth = (int)spec.cellWidth;
        data->tileHeight = (int)spec.cellHeight;
        TilesetSource source;
        source.id = 0;
        data->sources.push_back(source);
        TileDef& tile = data->addTile(0, 0, 0);
        tile.collision.mode = TileCollisionMode::Full;
        tile.collision.kind = TileCollisionKind::Solid;
        auto runtime = std::make_shared<TilesetRuntime>();
        runtime->data = data;
        runtime->pages = {"placement.png"};
        TileRenderInfo info;
        info.valid = true;
        info.u1 = 1.0f;
        info.v1 = 1.0f;
        info.x1 = (float)spec.cellWidth;
        info.y1 = (float)spec.cellHeight;
        info.width = (int)spec.cellWidth;
        info.height = (int)spec.cellHeight;
        runtime->tiles.assign((size_t)std::max(data->nextId, 1), TileRenderInfo());
        runtime->tiles[0] = info;
        return runtime;
    }

}

TEST(TilePlacement, DrawnColliderAndPickedCellAgree) {
    for (const PlacementCase& c : kPlacementCases) {
        SCOPED_TRACE(c.name);
        const TileGridSpec spec = placementGrid(c.layout);
        const std::shared_ptr<TilesetRuntime> tileset = placementTileset(spec);
        TileCells cells(8);
        cells.setTile(kPlacedCell, 0);

        // drawn: the chunk builder's instance, layer space (1 unit = 1 pixel here)
        TilemapChunkParams params;
        params.cells = &cells;
        params.grid = spec;
        params.tileset = tileset.get();
        params.pixelsPerUnit = 1.0;
        params.tileAnchorX = c.anchorX;
        params.tileAnchorY = c.anchorY;
        params.dualGrid = c.dualGrid;
        SpriteInstanceBatch batch;
        TilemapChunkBuild build;
        tilechunk::build(params, cells.chunkOf(kPlacedCell), batch, build);
        ASSERT_EQ(batch.getCount(), 1u);
        const Matrix4& m = batch.instances()[0].instanceMatrix;
        const double x0 = m[3][0], y0 = m[3][1], w = m[0][0], h = m[1][1];
        EXPECT_DOUBLE_EQ(x0, c.x0);
        EXPECT_DOUBLE_EQ(y0, c.y0);
        EXPECT_DOUBLE_EQ(w, spec.cellWidth);
        EXPECT_DOUBLE_EQ(h, spec.cellHeight);

        // collider: the full-cell shape covers the drawn image's box exactly
        TileColliderSettings settings;
        settings.cells = &cells;
        settings.tileset = tileset->data.get();
        settings.runtime = tileset.get();
        settings.grid = spec;
        settings.tileAnchor = TilePoint{c.anchorX, c.anchorY};
        settings.dualGrid = c.dualGrid;
        const TileColliderChunk collider = TileColliderBuilder(settings).build(cells.chunkOf(kPlacedCell));
        ASSERT_EQ(collider.loops.size(), 1u);
        double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
        for (const TilePoint& p : collider.loops[0].points) {
            minX = std::min(minX, p.x); minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
        }
        EXPECT_NEAR(minX, x0, 1e-6);
        EXPECT_NEAR(minY, y0, 1e-6);
        EXPECT_NEAR(maxX, x0 + w, 1e-6);
        EXPECT_NEAR(maxY, y0 + h, 1e-6);

        // picked: the drawn tile's centre names the cell, through a layer in a scene
        Scene scene;
        TilemapLayer layer(&scene);
        GridComponent grid;
        grid.layout = c.layout;
        const double ppu = (double)Engine::getPixelsPerUnit();
        grid.cellSize = Vector2((float)(spec.cellWidth / ppu), (float)(spec.cellHeight / ppu));
        scene.addComponent<GridComponent>(layer.getEntity(), grid);
        TilemapLayerComponent& component = layer.getComponent<TilemapLayerComponent>();
        component.tileAnchor = Vector2((float)c.anchorX, (float)c.anchorY);
        component.dualGrid = c.dualGrid;
        layer.setCell(kPlacedCell.x, kPlacedCell.y, 0);
        int tile = NO_TILE;
        CellCoord picked;
        EXPECT_TRUE(layer.getCellAt(Vector3((float)((x0 + w * 0.5) / ppu), (float)((y0 + h * 0.5) / ppu), 0.0f), tile, picked));
        EXPECT_EQ(tile, 0);
        EXPECT_EQ(picked, kPlacedCell);
        // below the drawn tile (where the logical cell is when the tiles moved up, or empty space)
        EXPECT_FALSE(layer.getCellAt(Vector3((float)((x0 + w * 0.5) / ppu), (float)((y0 - 1.0) / ppu), 0.0f), tile, picked));
    }
}

// the culling bounds of a chunk follow its tiles: a tileAnchor of two cells right moves them
TEST(TilePlacement, ChunkBoundsFollowTheTileShift) {
    const TileGridSpec spec = placementGrid(TileGridLayout::Rectangle);
    CellRect rect = CellRect::of(CellCoord{0, 0}, CellCoord{7, 7});
    const AABB still = tilechunk::chunkBounds(spec, rect, 0.0, 0.0, TilePoint{});
    const AABB moved = tilechunk::chunkBounds(spec, rect, 0.0, 0.0, tilegrid::tileShift(spec, 2.0, 0.0, false));
    EXPECT_FLOAT_EQ(still.getMinimum().x, 0.0f);
    EXPECT_FLOAT_EQ(still.getMaximum().x, 128.0f);
    EXPECT_FLOAT_EQ(moved.getMinimum().x, 32.0f);
    EXPECT_FLOAT_EQ(moved.getMaximum().x, 160.0f);
    const AABB dual = tilechunk::chunkBounds(spec, rect, 0.0, 0.0, tilegrid::tileShift(spec, 0.0, 0.0, true));
    EXPECT_FLOAT_EQ(dual.getMinimum().x, -8.0f);
    EXPECT_FLOAT_EQ(dual.getMaximum().y, 120.0f);
}
