// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The CPU composite (docs/specs/S50-grid-tilemap.md R76, R8, R83): placement, flips, transpose,
// painter's order and alpha over against golden images worked out by hand from the rules. A 2x2 or
// 2x3 atlas of letters stands in for pixels: each letter is a distinct opaque colour, '.' is
// transparent; golden rows read top to bottom.

#include "tilemap/TilemapComposite.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::tilemap;
using jasperengine::editor::pixelart::PixelRgba;

namespace {

    PixelRgba letter(char c) {
        return c == '.' ? PixelRgba{0, 0, 0, 0} : PixelRgba{(uint8_t)c, 0, 0, 255};
    }

    // a page from rows of letters (top row first)
    PixelImage pageOf(const std::vector<std::string>& rows) {
        PixelImage image((int)rows.front().size(), (int)rows.size());
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) image.set(x, y, letter(rows[(size_t)y][(size_t)x]));
        }
        return image;
    }

    std::vector<std::string> rowsOf(const PixelImage& image) {
        std::vector<std::string> rows;
        for (int y = 0; y < image.height(); y++) {
            std::string row;
            for (int x = 0; x < image.width(); x++) {
                const PixelRgba pixel = image.at(x, y);
                row += pixel[3] == 0 ? '.' : (char)pixel[0];
            }
            rows.push_back(row);
        }
        return rows;
    }

    // one texture page holding `page` cut into tiles of `tileW` x `tileH`, one per column;
    // tiles get ids 0, 1, ... left to right
    struct Fixture {
        PixelImage page;
        TilesetRuntime runtime;
        TilesetPagePixels pixels;

        Fixture(const std::vector<std::string>& rows, int tileW, int tileH, int tiles):
            page(pageOf(rows)),
            pixels([this](const std::string&, std::string*) -> std::optional<PixelImage> { return page; }) {
            auto data = std::make_shared<TilesetData>();
            data->tileWidth = tileW;
            data->tileHeight = tileH;
            runtime.pages = {"atlas.png"};
            runtime.pageWidths = {page.width()};
            runtime.pageHeights = {page.height()};
            runtime.tiles.resize((size_t)tiles);
            for (int i = 0; i < tiles; i++) {
                data->addTile(0, i, 0);
                TileRenderInfo& info = runtime.tiles[(size_t)i];
                info.valid = true;
                info.width = tileW;
                info.height = tileH;
                info.x1 = (float)tileW;
                info.y1 = (float)tileH;
                info.u0 = (float)(i * tileW) / (float)page.width();
                info.u1 = (float)((i + 1) * tileW) / (float)page.width();
                info.v1 = (float)tileH / (float)page.height();
            }
            runtime.data = data;
        }
    };

    CompositeParams baseParams(const TileCells& cells, const Fixture& fixture, TileGridLayout layout, int cellW, int cellH) {
        CompositeParams params;
        params.cells = &cells;
        params.tileset = &fixture.runtime;
        params.grid.layout = layout;
        params.grid.cellWidth = cellW;
        params.grid.cellHeight = cellH;
        return params;
    }

    std::vector<std::string> compositeRows(CompositeParams params, Fixture& fixture, int& originX, int& originY) {
        CompositeResult result;
        PixelProblem error;
        EXPECT_TRUE(compositeLayer(params, fixture.pixels, result, error)) << error.message;
        originX = result.originX;
        originY = result.originY;
        return rowsOf(result.image);
    }

}

// Tile "ABCD" as [A B / C D] (A top-left), one rectangle cell at the origin: the eight transform
// combinations, in Tiled's order (transpose first, then flip X, then flip Y).
TEST(TilemapComposite, FlipsAndTransposeFollowTiledsOrder) {
    struct Case {
        uint32_t flags;
        std::vector<std::string> golden;
    };
    const Case cases[] = {
        {0u, {"AB", "CD"}},
        {TILE_FLIP_X, {"BA", "DC"}},
        {TILE_FLIP_Y, {"CD", "AB"}},
        {TILE_FLIP_X | TILE_FLIP_Y, {"DC", "BA"}},
        // transpose: (x, y) -> (y, x): [A C / B D]; the flips then mirror that image
        {TILE_TRANSPOSE, {"AC", "BD"}},
        {TILE_TRANSPOSE | TILE_FLIP_X, {"CA", "DB"}},
        {TILE_TRANSPOSE | TILE_FLIP_Y, {"BD", "AC"}},
        {TILE_TRANSPOSE | TILE_FLIP_X | TILE_FLIP_Y, {"DB", "CA"}},
    };
    for (const Case& c : cases) {
        Fixture fixture({"AB", "CD"}, 2, 2, 1);
        TileCells cells;
        cells.setTile(CellCoord{0, 0}, 0, c.flags);
        CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
        params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
        int ox, oy;
        EXPECT_EQ(compositeRows(params, fixture, ox, oy), c.golden) << c.flags;
    }
}

TEST(TilemapComposite, RectangleAnchorsBottomLeftAndCoversEmptyCells) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    cells.setTile(CellCoord{1, 0}, 0);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{1, 1});
    int ox, oy;
    // the region is 2 x 2 cells: the bottom-right cell holds the tile, the rest is empty
    EXPECT_EQ(compositeRows(params, fixture, ox, oy), (std::vector<std::string>{"....", "....", "..AB", "..CD"}));
    EXPECT_EQ(ox, 0);
    EXPECT_EQ(oy, 4);
}

// R8: a tile larger than its cell stands on the cell's bottom-left corner and reaches up over the
// row above; the painter's order (rows from the top) draws it over that row, and alpha over keeps
// the row's pixels where its image is transparent
TEST(TilemapComposite, TallTileOverlapsTheRowAboveInThePaintersOrder) {
    // tile 0: 2x3 with a transparent top row; tile 1: 2x3 fully opaque with one letter
    Fixture fixture({"..xx", "AByy", "CDyy"}, 2, 3, 2);
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 0);
    cells.setTile(CellCoord{0, 1}, 1);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 1});
    int ox, oy;
    // cell (0,1) stands at y 2..5: its 3 rows from the top; tile 0 at y 0..3 is drawn after it, so
    // its opaque rows AB / CD cover y 0..2 and its transparent top row leaves y 2..3 showing tile 1
    EXPECT_EQ(compositeRows(params, fixture, ox, oy),
              (std::vector<std::string>{"xx", "yy", "yy", "AB", "CD"}));
    EXPECT_EQ(oy, 5);
}

TEST(TilemapComposite, IsometricAnchorsTheImageAtTheBottomVertexCentre) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 0);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Isometric, 4, 2);
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    int ox, oy;
    // the diamond of cell (0, 0) spans x -2..2, y 0..2; the image is centred on x 0 at y 0
    EXPECT_EQ(compositeRows(params, fixture, ox, oy), (std::vector<std::string>{".AB.", ".CD."}));
    EXPECT_EQ(ox, -2);
    EXPECT_EQ(oy, 2);
}

TEST(TilemapComposite, TileOffsetAndTileAnchorMoveTheImage) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    fixture.runtime.tiles[0].offsetX = 1;
    fixture.runtime.tiles[0].offsetY = 1;
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 0);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 4, 4);
    params.tileAnchorX = 0.25;   // one pixel right of the offset above, in cells of 4 pixels
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    int ox, oy;
    EXPECT_EQ(compositeRows(params, fixture, ox, oy), (std::vector<std::string>{"....", "..AB", "..CD", "...."}));
}

TEST(TilemapComposite, DualGridDrawsHalfACellDownLeft) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 0);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.dualGrid = true;
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    int ox, oy;
    EXPECT_EQ(compositeRows(params, fixture, ox, oy), (std::vector<std::string>{"AB", "CD"}));
    EXPECT_EQ(ox, -1);
    EXPECT_EQ(oy, 1);
}

TEST(TilemapComposite, ScaleRepeatsPixels) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 0);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.scale = 2;
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    int ox, oy;
    EXPECT_EQ(compositeRows(params, fixture, ox, oy), (std::vector<std::string>{"AABB", "AABB", "CCDD", "CCDD"}));
}

TEST(TilemapComposite, SameArgumentsGiveTheSameBytes) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) cells.setTile(CellCoord{x, y}, 0, x % 2 ? TILE_FLIP_X : TILE_FLIP_Y);
    }
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Hexagon, 4, 4);
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{3, 3});
    CompositeResult a, b;
    PixelProblem error;
    ASSERT_TRUE(compositeLayer(params, fixture.pixels, a, error));
    ASSERT_TRUE(compositeLayer(params, fixture.pixels, b, error));
    EXPECT_EQ(a.image.hash(), b.image.hash());
    EXPECT_FALSE(a.image.empty());
}

TEST(TilemapComposite, MissingTilesAreCounted) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    cells.setTile(CellCoord{0, 0}, 9);
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    CompositeResult out;
    PixelProblem error;
    ASSERT_TRUE(compositeLayer(params, fixture.pixels, out, error));
    EXPECT_EQ(out.missing, 1u);
    EXPECT_EQ(rowsOf(out.image), (std::vector<std::string>{"..", ".."}));
}

TEST(TilemapComposite, ScaleOutsideTheRangeIsTyped) {
    Fixture fixture({"AB", "CD"}, 2, 2, 1);
    TileCells cells;
    CompositeParams params = baseParams(cells, fixture, TileGridLayout::Rectangle, 2, 2);
    params.scale = 9;
    params.region = CellRect::of(CellCoord{0, 0}, CellCoord{0, 0});
    CompositeResult out;
    PixelProblem error;
    EXPECT_FALSE(compositeLayer(params, fixture.pixels, out, error));
    EXPECT_EQ(error.field, "scale");
}
