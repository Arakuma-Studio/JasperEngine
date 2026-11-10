// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// "Create Tilemap from Aseprite" (docs/specs/S50-grid-tilemap.md R12, R48): the conversion of an
// AsepriteImport's tilesets and tilemap cels into a plan, on hand-made import outputs.

#include "tilemap/import/TilemapFromAseprite.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::tilemapimport;
namespace aseprite = jasperengine::editor::pixelart::aseprite;

namespace {

    aseprite::AsepriteImport makeImport() {
        aseprite::AsepriteImport import;
        aseprite::TilesetOutput tileset;
        tileset.name = "ground";
        tileset.id = 7;
        tileset.tileWidth = 16;
        tileset.tileHeight = 16;
        tileset.tileCount = 3;
        tileset.tileUserData.push_back(nullptr);
        tileset.tileUserData.push_back({{"text", "solid"}, {"properties", {{"damage", 2}}}});
        import.tilesetOutputs.push_back(std::move(tileset));

        aseprite::TilemapCelOutput cel;
        cel.layer = "Tiles";
        cel.frame = 0;
        cel.x = 32;
        cel.y = 16;
        cel.tileset = 7;
        cel.tilemap.columns = 2;
        cel.tilemap.rows = 2;
        // row 0 (top): tile 1 flipped X, empty; row 1: tile 2 with y flip and diagonal, tile 1
        cel.tilemap.tiles = {1u | cel.tilemap.xFlipMask, 0u, 2u | cel.tilemap.yFlipMask | cel.tilemap.diagonalMask, 1u};
        import.tilemapOutputs.push_back(cel);
        cel.frame = 1;
        import.tilemapOutputs.push_back(cel);
        return import;
    }

    const ImportCell* cellAt(const ImportLayer& layer, int x, int y) {
        for (const ImportCell& c : layer.cells) {
            if (c.x == x && c.y == y) return &c;
        }
        return nullptr;
    }

}

TEST(TilemapFromAsepriteTest, CellsAndFlipsFromTheFirstFrame) {
    ImportPlan plan;
    std::vector<ImportProblem> problems;
    ASSERT_TRUE(tilemapFromAseprite(makeImport(), "art/world.aseprite", 64, 0, plan, problems));
    EXPECT_EQ(plan.format, "aseprite");
    ASSERT_EQ(plan.grids.size(), 1u);
    EXPECT_EQ(plan.grids[0].name, "world");
    EXPECT_EQ(plan.grids[0].spec.cellWidth, 16.0);
    ASSERT_EQ(plan.grids[0].layers.size(), 1u);
    const ImportLayer& layer = plan.grids[0].layers[0];
    EXPECT_EQ(layer.name, "Tiles");
    EXPECT_EQ(layer.tileset, 0);
    ASSERT_EQ(layer.cells.size(), 3u);
    // the canvas is 64 high: the cel at y = 16 spans 2 rows of 16, its bottom edge is 16 above the
    // canvas bottom: Aseprite's (2, top row) is our (2, 2), (3, bottom row) is (3, 1), (2, bottom) (2, 1)
    const ImportCell* a = cellAt(layer, 2, 2);
    ASSERT_TRUE(a);
    EXPECT_EQ(cellValueTile(a->value), 0);
    EXPECT_EQ(cellValueFlags(a->value), TILE_FLIP_X);
    const ImportCell* b = cellAt(layer, 2, 1);
    ASSERT_TRUE(b);
    EXPECT_EQ(cellValueTile(b->value), 1);
    EXPECT_EQ(cellValueFlags(b->value), TILE_FLIP_Y | TILE_TRANSPOSE);
    const ImportCell* c = cellAt(layer, 3, 1);
    ASSERT_TRUE(c);
    EXPECT_EQ(cellValueTile(c->value), 0);
    EXPECT_EQ(cellValueFlags(c->value), 0u);
    EXPECT_EQ(cellAt(layer, 3, 2), nullptr);
    EXPECT_EQ(layer.offsetX, 0.0);
    EXPECT_EQ(layer.offsetY, 0.0);
    // the second frame's cel is reported, not imported
    ASSERT_EQ(plan.warnings.size(), 1u);
}

TEST(TilemapFromAsepriteTest, AsepriteTilesetSourceAndUserData) {
    ImportPlan plan;
    std::vector<ImportProblem> problems;
    ASSERT_TRUE(tilemapFromAseprite(makeImport(), "art/world.aseprite", 64, 0, plan, problems));
    ASSERT_EQ(plan.tilesets.size(), 1u);
    const TilesetData& data = plan.tilesets[0].data;
    EXPECT_EQ(data.name, "ground");
    ASSERT_EQ(data.sources.size(), 1u);
    EXPECT_EQ(data.sources[0].kind, TilesetSourceKind::Aseprite);
    EXPECT_EQ(data.sources[0].asset.path, "art/world.aseprite");
    EXPECT_EQ(data.sources[0].asepriteTileset, 0);
    // Aseprite's tile 0 is empty: tiles 1 and 2 are our 0 and 1, positions along the strip
    ASSERT_EQ(data.tiles.size(), 2u);
    EXPECT_EQ(data.nextId, 2);
    EXPECT_EQ(data.tiles[1].id, 1);
    EXPECT_EQ(data.tiles[1].atX, 1);
    // Aseprite's tile 1's user data (index 1 of tileUserData) becomes our tile 0's properties
    ASSERT_EQ(data.tiles[0].properties.size(), 2u);
    EXPECT_EQ(data.tiles[0].properties.at("text").s, "solid");
    EXPECT_EQ(data.tiles[0].properties.at("damage").i, 2);
    EXPECT_TRUE(data.tiles[1].properties.empty());
    ASSERT_EQ(data.propertySchema.size(), 2u);
}

TEST(TilemapFromAsepriteTest, NoTilemapsFails) {
    aseprite::AsepriteImport import;
    ImportPlan plan;
    std::vector<ImportProblem> problems;
    EXPECT_FALSE(tilemapFromAseprite(import, "x.aseprite", 16, 0, plan, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "parse");
}
