// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Tileset editing (docs/specs/S50-grid-tilemap.md R12, R13, R15, R20, R71): slicing, re-slicing with
// stable ids (R13's red control: a build numbering by position fails ResliceKeepsIdsAndAppendsNew),
// the alpha collision outlines of a fixture PNG, sheet and Aseprite sources, the contact sheet.

#include "tilemap/TilesetEdit.h"

#include "pixelart/aseprite/AsepriteImport.h"
#include "sprite/SpriteSheet.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;
using jasperengine::editor::pixelart::PixelRgba;

namespace {

    constexpr PixelRgba kSolid{255, 255, 255, 255};
    constexpr int kTile = 4;

    // a texture of tile slots: opaque where `solid` has a '#', cell by cell, top row first
    PixelImage makeAtlas(const std::vector<std::string>& solid) {
        const int rows = (int)solid.size(), columns = (int)solid.front().size();
        PixelImage image(columns * kTile, rows * kTile);
        for (int row = 0; row < rows; row++) {
            for (int column = 0; column < columns; column++) {
                if (solid[(size_t)row][(size_t)column] != '#') continue;
                for (int y = 0; y < kTile; y++) {
                    for (int x = 0; x < kTile; x++) image.set(column * kTile + x, row * kTile + y, kSolid);
                }
            }
        }
        return image;
    }

    TextureSlicing slicing(bool skipEmpty = true) {
        TextureSlicing s;
        s.tileWidth = kTile;
        s.tileHeight = kTile;
        s.skipEmpty = skipEmpty;
        return s;
    }

    TileDef findAt(const TilesetData& data, int column, int row) {
        for (const TileDef& tile : data.tiles) {
            if (tile.atX == column && tile.atY == row) return tile;
        }
        ADD_FAILURE() << "no tile at " << column << "," << row;
        return TileDef();
    }

}

TEST(TilesetEdit, SliceGivesReadingOrderIdsAndSkipsEmpty) {
    TilesetData data;
    PixelProblem error;
    ASSERT_TRUE(createFromTexture("t", AssetRef("t.png"), slicing(), makeAtlas({"#.#", "##."}), data, error));
    ASSERT_EQ(data.tiles.size(), 4u);
    EXPECT_EQ(findAt(data, 0, 0).id, 0);
    EXPECT_EQ(findAt(data, 2, 0).id, 1);
    EXPECT_EQ(findAt(data, 0, 1).id, 2);
    EXPECT_EQ(findAt(data, 1, 1).id, 3);
    EXPECT_EQ(data.nextId, 4);
}

TEST(TilesetEdit, SliceHonoursMarginAndSpacing) {
    // 2 columns of 4 px with a margin of 1 and a spacing of 2: 1 + 4 + 2 + 4 + 1 = 12 wide
    PixelImage image(12, 6);
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 12; x++) image.set(x, y, kSolid);
    }
    TextureSlicing s = slicing();
    s.margin = 1;
    s.spacing = 2;
    TilesetData data;
    PixelProblem error;
    ASSERT_TRUE(createFromTexture("t", AssetRef("t.png"), s, image, data, error));
    EXPECT_EQ(data.tiles.size(), 2u);
    EXPECT_EQ(data.sources[0].margin, 1);
    EXPECT_EQ(data.sources[0].spacing, 2);
}

// R13: re-slicing keeps the ids of tiles at the same source position and appends new ones, so a
// cell's id still shows the same art. A build that numbers by position fails: it gives the new
// (1, 0) slot id 1 and shifts the rest.
TEST(TilesetEdit, ResliceKeepsIdsAndAppendsNew) {
    TilesetData data;
    PixelProblem error;
    ASSERT_TRUE(createFromTexture("t", AssetRef("t.png"), slicing(), makeAtlas({"#.", "##"}), data, error));
    ASSERT_EQ(data.tiles.size(), 3u);
    const int32_t idAt00 = findAt(data, 0, 0).id;
    const int32_t idAt01 = findAt(data, 0, 1).id;
    const int32_t idAt11 = findAt(data, 1, 1).id;
    data.findTile(idAt11)->name = "corner";

    // the empty slot is painted, and the texture grows a column
    ResliceReport report;
    ASSERT_TRUE(resliceTexture(data, 0, slicing(), makeAtlas({"###", "###"}), report, error));
    EXPECT_EQ(findAt(data, 0, 0).id, idAt00);
    EXPECT_EQ(findAt(data, 0, 1).id, idAt01);
    EXPECT_EQ(findAt(data, 1, 1).id, idAt11);
    EXPECT_EQ(findAt(data, 1, 1).name, "corner");
    EXPECT_EQ(report.removed.size(), 0u);
    ASSERT_EQ(report.added.size(), 3u);
    EXPECT_EQ(findAt(data, 1, 0).id, 3) << "the new slot takes a new id, not its position's";
    EXPECT_EQ(report.added[0], 3);
    EXPECT_EQ(data.nextId, 6);
}

TEST(TilesetEdit, ResliceRemovesTilesWhoseSlotIsGoneAndDoesNotReuseTheirIds) {
    TilesetData data;
    PixelProblem error;
    ASSERT_TRUE(createFromTexture("t", AssetRef("t.png"), slicing(), makeAtlas({"##", "##"}), data, error));
    ResliceReport report;
    ASSERT_TRUE(resliceTexture(data, 0, slicing(), makeAtlas({"##"}), report, error));
    EXPECT_EQ(report.removed, (std::vector<int32_t>{2, 3}));
    EXPECT_EQ(data.tiles.size(), 2u);
    EXPECT_EQ(data.nextId, 4) << "ids are never reused";
    ASSERT_TRUE(resliceTexture(data, 0, slicing(), makeAtlas({"##", "#."}), report, error));
    EXPECT_EQ(findAt(data, 0, 1).id, 4);
}

TEST(TilesetEdit, ResliceKeepsTheIdOfATileThatBecameTransparent) {
    TilesetData data;
    PixelProblem error;
    ASSERT_TRUE(createFromTexture("t", AssetRef("t.png"), slicing(), makeAtlas({"##"}), data, error));
    ResliceReport report;
    ASSERT_TRUE(resliceTexture(data, 0, slicing(), makeAtlas({"#."}), report, error));
    EXPECT_EQ(report.kept, (std::vector<int32_t>{0, 1}));
    EXPECT_TRUE(report.removed.empty());
}

TEST(TilesetEdit, SheetSourceTakesFramesByName) {
    SpriteSheetData sheet;
    SpriteFrame frame;
    frame.name = "water_0";
    frame.sourceSize = Vector2(8, 8);
    frame.rect = Rect(0, 0, 8, 8);
    sheet.frames.push_back(frame);
    frame.name = "water_1";
    sheet.frames.push_back(frame);
    TilesetData data;
    int source = -1;
    PixelProblem error;
    ASSERT_TRUE(addSheetSource(data, AssetRef("sheet.spriteatlas"), sheet, {"water_1"}, source, error));
    ASSERT_EQ(data.tiles.size(), 1u);
    EXPECT_EQ(data.tiles[0].frame, "water_1");
    EXPECT_FALSE(addSheetSource(data, AssetRef("sheet.spriteatlas"), sheet, {"lava"}, source, error));
    EXPECT_EQ(error.kind, "unknown_tile");
}

TEST(TilesetEdit, AsepriteSourceSkipsTileZeroAndReadsUserData) {
    jasperengine::editor::pixelart::aseprite::TilesetOutput output;
    output.name = "tiles";
    output.tileWidth = 8;
    output.tileHeight = 8;
    output.tileCount = 3;
    output.tileUserData = {nlohmann::json(), nlohmann::json::object(), nlohmann::json{{"text", "water"}}};
    TilesetData data;
    data.addTile(0, 0, 0);    // an earlier tile: the source's tiles continue after it
    int source = -1;
    std::vector<std::string> warnings;
    PixelProblem error;
    ASSERT_TRUE(addAsepriteSource(data, AssetRef("a.aseprite"), output, 2, warnings, source, error));
    ASSERT_EQ(data.tiles.size(), 3u);
    const TileDef* first = data.findTile(1);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->atX, 0) << "Aseprite tile 1 is our tile 0 of the source";
    const TileDef* second = data.findTile(2);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->atX, 1);
    ASSERT_EQ(second->properties.count("text"), 1u);
    EXPECT_EQ(second->properties.at("text").s, "water");
    ASSERT_NE(data.findProperty("text"), nullptr);
    EXPECT_EQ(data.sources[0].asepriteTileset, 2);
}

// A real PNG read through sprite::loadSpriteImage (the page cache's default loader) and traced: the
// outlines are in tile pixels, y up, from the tile's bottom-left corner.
TEST(TilesetEdit, AlphaCollisionsOfAFixturePng) {
    auto data = std::make_shared<TilesetData>();
    data->tileWidth = 8;
    data->tileHeight = 8;
    TilesetSource source;
    source.asset = AssetRef("tilemaps/alpha_tiles.png");
    data->sources.push_back(source);
    TileDef& first = data->addTile(0, 0, 0);
    first.collision.mode = TileCollisionMode::Alpha;
    TileDef& second = data->addTile(0, 1, 0);
    second.collision.mode = TileCollisionMode::Alpha;
    TilesetRuntime runtime;
    runtime.pages = {source.asset.path};
    runtime.pageWidths = {16};
    runtime.pageHeights = {8};
    runtime.tiles.resize(2);
    for (int i = 0; i < 2; i++) {
        TileRenderInfo& info = runtime.tiles[(size_t)i];
        info.valid = true;
        info.width = 8;
        info.height = 8;
        info.x1 = 8;
        info.y1 = 8;
        info.u0 = (float)i * 0.5f;
        info.u1 = info.u0 + 0.5f;
        info.v1 = 1.0f;
    }
    runtime.data = data;

    TilesetPagePixels pages;
    size_t filled = 0;
    PixelProblem error;
    ASSERT_TRUE(fillAlphaCollisions(*data, runtime, pages, {}, filled, error)) << error.message;
    EXPECT_EQ(filled, 2u);

    // tile 0: the opaque block is columns 2-5, rows 2-5 of the image, so y-up from 2 to 6
    ASSERT_EQ(data->tiles[0].collision.polygons.size(), 1u);
    using Points = std::vector<std::array<int32_t, 2>>;
    const Points blockOutline = {{{2, 2}}, {{6, 2}}, {{6, 6}}, {{2, 6}}};
    EXPECT_EQ(data->tiles[0].collision.polygons[0].points, blockOutline);
    // tile 1 is opaque throughout: its cell's outline
    ASSERT_EQ(data->tiles[1].collision.polygons.size(), 1u);
    const Points fullOutline = {{{0, 0}}, {{8, 0}}, {{8, 8}}, {{0, 8}}};
    EXPECT_EQ(data->tiles[1].collision.polygons[0].points, fullOutline);
}

TEST(TilesetEdit, AlphaCollisionsFailOnAMissingPage) {
    auto data = std::make_shared<TilesetData>();
    TileDef& tile = data->addTile(0, 0, 0);
    tile.collision.mode = TileCollisionMode::Alpha;
    TilesetRuntime runtime;
    runtime.data = data;
    runtime.tiles.resize(1);
    runtime.pages = {"missing.png"};
    runtime.pageWidths = {8};
    runtime.pageHeights = {8};
    runtime.tiles[0].valid = true;
    runtime.tiles[0].width = 8;
    runtime.tiles[0].height = 8;
    TilesetPagePixels pages([](const std::string&, std::string* e) -> std::optional<PixelImage> {
        if (e) *e = "no such page";
        return std::nullopt;
    });
    size_t filled = 0;
    PixelProblem error;
    EXPECT_FALSE(fillAlphaCollisions(*data, runtime, pages, {}, filled, error));
    EXPECT_EQ(error.kind, "tileset_missing");
    EXPECT_TRUE(tile.collision.polygons.empty());
}

TEST(TilesetEdit, ContactSheetIsDeterministicAndLabelsIds) {
    auto data = std::make_shared<TilesetData>();
    data->addTile(0, 0, 0);
    data->addTile(0, 1, 0);
    PixelImage atlas = makeAtlas({"##"});
    TilesetRuntime runtime;
    runtime.data = data;
    runtime.pages = {"t.png"};
    runtime.tiles.resize(2);
    for (int i = 0; i < 2; i++) {
        TileRenderInfo& info = runtime.tiles[(size_t)i];
        info.valid = true;
        info.width = kTile;
        info.height = kTile;
        info.x1 = (float)kTile;
        info.y1 = (float)kTile;
        info.u0 = (float)i * 0.5f;
        info.u1 = info.u0 + 0.5f;
        info.v1 = 1.0f;
    }
    TilesetPagePixels pages([&](const std::string&, std::string*) -> std::optional<PixelImage> { return atlas; });
    ContactSheetOptions options;
    options.columns = 2;
    options.scale = 2;
    const PixelImage first = contactSheet(runtime, pages, options);
    EXPECT_EQ(first.width(), 2 * (2 * kTile + 1) + 1);
    EXPECT_EQ(first.height(), 2 * kTile + 7 + 2);
    EXPECT_EQ(first, contactSheet(runtime, pages, options));
    // the tile's corner, then the digit 0's top-left pixel below it
    EXPECT_EQ(first.at(1, 1), kSolid);
    EXPECT_EQ(first.at(1, 1 + 2 * kTile + 1), (PixelRgba{255, 255, 255, 255}));
}

TEST(TilesetEdit, CoverageImagesDrawMasks) {
    TilesetData data;
    TerrainSetDef set;
    set.name = "Ground";
    set.mode = TerrainMode::Sides;
    set.terrains.push_back(TerrainDef{"Grass", "#102030"});
    data.terrainSets.push_back(set);
    TerrainCoverage coverage;
    TerrainMissingMask mask;
    mask.center = 0;
    mask.peering.fill(PEERING_NONE);
    mask.peering[0] = 0;   // n
    coverage.missing.push_back(mask);
    const std::vector<PixelImage> images = coverageImages(data, 0, coverage, 2);
    ASSERT_EQ(images.size(), 1u);
    ASSERT_EQ(images[0].width(), 6);
    const PixelRgba grass{16, 32, 48, 255};
    EXPECT_EQ(images[0].at(2, 2), grass) << "the centre cell";
    EXPECT_EQ(images[0].at(2, 0), grass) << "the north slot";
    EXPECT_NE(images[0].at(0, 0), grass) << "nw is none";
}

TEST(TilesetEdit, TilesetPathReplacesTheTexturesExtension) {
    EXPECT_EQ(tilesetPathForTexture("tiles/grass.png"), "tiles/grass.tileset");
}

TEST(TilesetEdit, ScaleNearestRepeatsPixels) {
    PixelImage image(1, 1);
    image.set(0, 0, kSolid);
    const PixelImage scaled = scaleNearest(image, 3);
    EXPECT_EQ(scaled.width(), 3);
    EXPECT_EQ(scaled.at(2, 2), kSolid);
}
