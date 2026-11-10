// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Tiled importer (docs/specs/S50-grid-tilemap.md AC8, R44): every orientation against Tiled's
// documented pixel positions, every encoding, infinite maps, external and collection tilesets,
// wang sets, the skipped items, and malformed files. Goldens and pixel-position formulas are
// hand-derived (editor/tests/fixtures/tilemaps/gen_fixtures.py makes the encoded fixtures).

#include "ImportPlanDump.h"

#include "tilemap/import/ImportPlan.h"
#include "tilemap/import/TiledImporter.h"

#include "tilemap/TileGridLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::tilemapimport;
using namespace jasperengine::editor::tilemapimport::testing;
namespace fs = std::filesystem;

namespace {

    ImportPlan parseFixture(const std::string& name, const FileReader& reader = fixtureReader()) {
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        const bool ok = parseTiled(readFixture(name), name, reader, plan, problems);
        EXPECT_TRUE(ok) << name << ": " << (problems.empty() ? std::string() : problems[0].field + ": " + problems[0].message);
        return plan;
    }

    // Tiled's pixel position of a cell's R8 anchor, y down, from its renderers
    // (IsometricRenderer::tileToScreenCoords, HexagonalRenderer's RenderParams and tileToScreenCoords)
    TilePoint tiledAnchor(const nlohmann::json& map, int x, int y) {
        const std::string orientation = map.at("orientation");
        const int w = map.at("width"), h = map.at("height");
        const int tw = map.at("tilewidth"), th = map.at("tileheight");
        if (orientation == "orthogonal") return {double(x * tw), double(y * th + th)};
        if (orientation == "isometric") {
            const double originX = h * tw / 2.0;
            return {originX + (x - y) * tw / 2.0, (x + y) * th / 2.0 + th};
        }
        const bool staggerX = map.value("staggeraxis", "y") == "x";
        const bool staggerEven = map.value("staggerindex", "odd") == "even";
        const int side = orientation == "hexagonal" ? map.value("hexsidelength", 0) : 0;
        const int tileW = tw & ~1, tileH = th & ~1;
        const int sideX = staggerX ? side : 0;
        const int sideY = staggerX ? 0 : side;
        const int sideOffsetX = (tileW - sideX) / 2, sideOffsetY = (tileH - sideY) / 2;
        const int columnWidth = sideOffsetX + sideX, rowHeight = sideOffsetY + sideY;
        int px, py;
        if (staggerX) {
            py = y * (tileH + sideY);
            if ((x & 1) ^ (staggerEven ? 1 : 0)) py += rowHeight;
            px = x * columnWidth;
        } else {
            px = x * (tileW + sideX);
            if ((y & 1) ^ (staggerEven ? 1 : 0)) px += columnWidth;
            py = y * rowHeight;
        }
        (void)w;
        return {px + tileW / 2.0, double(py + tileH)};
    }

    void expectOrientation(const std::string& stem) {
        SCOPED_TRACE(stem);
        const nlohmann::json map = goldenJson(stem + ".tmj");
        const int width = map.at("width");
        for (const std::string& file : {stem + ".tmj", stem + ".tmx"}) {
            SCOPED_TRACE(file);
            const ImportPlan plan = parseFixture(file);
            ASSERT_EQ(plan.grids.size(), 1u);
            const ImportGrid& grid = plan.grids[0];
            ASSERT_EQ(grid.layers.size(), 1u);
            const std::vector<ImportCell>& cells = grid.layers[0].cells;
            ASSERT_EQ(cells.size(), static_cast<size_t>(width) * map.at("height").get<int>());
            // our anchors and Tiled's (y flipped to y up) differ by one translation per map
            bool first = true;
            TilePoint delta;
            for (const ImportCell& cell : cells) {
                const int id = cellValueTile(cell.value);
                const int tiledX = id % width;
                const int tiledY = id / width;
                const TilePoint tiled = tiledAnchor(map, tiledX, tiledY);
                const TilePoint ours = tilegrid::cellToLocal(grid.spec, CellCoord{cell.x, cell.y});
                const double dx = ours.x - tiled.x;
                const double dy = ours.y + tiled.y;
                if (first) {
                    delta = {dx, dy};
                    first = false;
                }
                EXPECT_DOUBLE_EQ(dx, delta.x) << "cell " << tiledX << "," << tiledY;
                EXPECT_DOUBLE_EQ(dy, delta.y) << "cell " << tiledX << "," << tiledY;
            }
        }
    }

    void expectGolden(const std::string& file, const std::string& golden, const FileReader& reader = fixtureReader()) {
        const ImportPlan plan = parseFixture(file, reader);
        const nlohmann::json actual = planJson(plan);
        const nlohmann::json expected = goldenJson(golden);
        EXPECT_EQ(actual, expected) << file << "\nactual: " << actual.dump(1) << "\nexpected: " << expected.dump(1);
    }

    ImportProblem failure(const std::string& text, const FileReader& reader = FileReader()) {
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        const bool ok = parseTiled(text, "bad.tmj", reader, plan, problems);
        EXPECT_FALSE(ok) << text;
        return problems.empty() ? ImportProblem() : problems[0];
    }

    std::string minimalMap(const std::string& layerData, const std::string& extra = "\"encoding\":\"base64\",", int width = 2, int height = 1) {
        return R"({"type":"map","orientation":"orthogonal","width":)" + std::to_string(width) + R"(,"height":)" + std::to_string(height) +
               R"(,"tilewidth":16,"tileheight":16,"tilesets":[],"layers":[{"type":"tilelayer","name":"L","width":)" + std::to_string(width) + R"(,"height":)" +
               std::to_string(height) + "," + extra + R"("data":)" + layerData + "}]}";
    }

}

TEST(TiledImporterTest, EveryEncodingGivesTheSameCells) {
    const char* encodings[] = {"csv", "base64", "zlib", "gzip", "zstd"};
    for (const char* encoding : encodings) {
        SCOPED_TRACE(encoding);
        expectGolden(std::string("ortho_") + encoding + ".tmj", "ortho_encoded.expected.json");
        expectGolden(std::string("ortho_") + encoding + ".tmx", "ortho_encoded.expected.json");
    }
}

TEST(TiledImporterTest, FlipBitsAndNames) {
    const ImportPlan plan = parseFixture("ortho_csv.tmj");
    ASSERT_EQ(plan.grids.size(), 1u);
    EXPECT_EQ(plan.format, "tiled");
    EXPECT_EQ(plan.grids[0].name, "ortho_csv");
    EXPECT_EQ(plan.grids[0].layers[0].name, "Ground");
    // Tiled's row 0 is our top row: the same flipped gid 3|H at Tiled (3, 0) is our (3, 2)
    bool found = false;
    for (const ImportCell& c : plan.grids[0].layers[0].cells) {
        if (c.x == 3 && c.y == 2) {
            found = true;
            EXPECT_EQ(cellValueTile(c.value), 2);
            EXPECT_EQ(cellValueFlags(c.value), TILE_FLIP_X);
        }
    }
    EXPECT_TRUE(found);
}

TEST(TiledImporterTest, InfiniteChunks) {
    expectGolden("infinite_zlib.tmj", "infinite.expected.json");
    expectGolden("infinite_csv.tmx", "infinite.expected.json");
}

TEST(TiledImporterTest, FeaturesEqualGolden) {
    expectGolden("features.tmj", "features.expected.json");
    expectGolden("features.tmx", "features.expected.json");
}

TEST(TiledImporterTest, WangSetsEqualGolden) {
    expectGolden("wang.tmj", "wang.expected.json");
}

TEST(TiledImporterTest, IsometricWangDirectionsFollowTheAxisSwap) {
    expectGolden("iso_wang.tmj", "iso_wang.expected.json");
}

TEST(TiledImporterTest, ImageCollection) {
    expectGolden("collection.tmj", "collection.expected.json");
}

TEST(TiledImporterTest, ExternalTilesets) {
    {
        const ImportPlan plan = parseFixture("external_tsj.tmj");
        ASSERT_EQ(plan.tilesets.size(), 1u);
        EXPECT_EQ(plan.tilesets[0].externalFile, "tilesets/ext_tiles.tsj");
        nlohmann::json actual = planJson(plan);
        nlohmann::json expected = goldenJson("external.expected.json");
        expected["tilesets"][0]["external"] = "tilesets/ext_tiles.tsj";
        EXPECT_EQ(actual, expected) << actual.dump(1);
    }
    {
        const ImportPlan plan = parseFixture("external_tsx.tmx");
        ASSERT_EQ(plan.tilesets.size(), 1u);
        EXPECT_EQ(plan.tilesets[0].externalFile, "tilesets/ext_tiles.tsx");
        nlohmann::json actual = planJson(plan);
        nlohmann::json expected = goldenJson("external.expected.json");
        expected["tilesets"][0]["external"] = "tilesets/ext_tiles.tsx";
        EXPECT_EQ(actual, expected) << actual.dump(1);
    }
}

TEST(TiledImporterTest, HexRotationReportedAndCellsUnrotated) {
    expectGolden("hex_rotation.tmj", "hex_rotation.expected.json");
}

TEST(TiledImporterTest, IsometricCellsLandOnTiledPixels) {
    expectOrientation("iso_w4h3");
    expectOrientation("iso_w3h4");
}

TEST(TiledImporterTest, StaggeredCellsLandOnTiledPixels) {
    for (const char* stem : {"staggered_y_odd_h3", "staggered_y_odd_h4", "staggered_y_even_h3", "staggered_y_even_h4", "staggered_x_odd", "staggered_x_even"}) expectOrientation(stem);
}

TEST(TiledImporterTest, HexagonalCellsLandOnTiledPixels) {
    for (const char* stem : {"hex_y_odd_h3", "hex_y_odd_h4", "hex_y_even_h3", "hex_y_even_h4", "hex_x_odd", "hex_x_even"}) expectOrientation(stem);
}

TEST(TiledImporterTest, MissingExternalTilesetIsTyped) {
    const ImportProblem p = failure(R"({"type":"map","orientation":"orthogonal","width":1,"height":1,"tilewidth":16,"tileheight":16,"tilesets":[{"firstgid":1,"source":"nope.tsj"}],"layers":[]})",
                                    [](const std::string&, std::string&) { return false; });
    EXPECT_EQ(p.kind, "tileset_missing");
    EXPECT_NE(p.field.find("tilesets[0].source"), std::string::npos) << p.field;
}

TEST(TiledImporterTest, MalformedFilesGiveParseErrorsWithLocation) {
    {
        const ImportProblem p = failure("{\"type\":\"map\",\n\"orientation\": ");
        EXPECT_EQ(p.kind, "parse");
        EXPECT_EQ(p.line, 2) << p.message;
    }
    {
        const ImportProblem p = failure("<?xml version=\"1.0\"?>\n<map><layer></map>");
        EXPECT_EQ(p.kind, "parse");
        EXPECT_EQ(p.line, 2) << p.message;
    }
    {
        // a bad CSV line of a .tmx names its element's line
        const std::string tmx = "<?xml version=\"1.0\"?>\n<map orientation=\"orthogonal\" width=\"2\" height=\"1\" tilewidth=\"16\" tileheight=\"16\">\n <layer name=\"L\" width=\"2\" height=\"1\">\n  <data encoding=\"csv\">1,oops</data>\n </layer>\n</map>";
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        EXPECT_FALSE(parseTiled(tmx, "bad.tmx", FileReader(), plan, problems));
        ASSERT_EQ(problems.size(), 1u);
        EXPECT_EQ(problems[0].kind, "parse");
        EXPECT_EQ(problems[0].line, 3) << problems[0].message;
    }
    const struct {
        std::string text;
        const char* field;
    } cases[] = {
        {R"({"type":"map","orientation":"orthogonal","width":2,"height":1,"tilesets":[],"layers":[]})", "tilewidth"},
        {R"({"type":"map","orientation":"rhombus","width":2,"height":1,"tilewidth":16,"tileheight":16,"tilesets":[],"layers":[]})", "orientation"},
        {R"({"type":"map","orientation":"orthogonal","width":1000000000,"height":1000000000,"tilewidth":16,"tileheight":16,"tilesets":[],"layers":[]})", "width"},
        {R"({"type":"map","orientation":"orthogonal","width":2,"height":1,"tilewidth":16,"tileheight":16,"tilesets":[],"layers":5})", "layers"},
        {R"({"type":"tileset","tilewidth":16,"tileheight":16})", "type"},
        {minimalMap("[1]", ""), "layers[0].data"},
        {minimalMap("[1, -5]", ""), "layers[0].data[1]"},
        {minimalMap("\"%%%\""), "layers[0].data"},
        {minimalMap("\"AAAA\"", "\"encoding\":\"base64\",\"compression\":\"lz4\","), "layers[0].data"},
        {minimalMap("\"AAAAAAAA\"", "\"encoding\":\"base64\",\"compression\":\"zlib\","), "layers[0].data"},
        {minimalMap("\"H4sIAAAAAAAA\"", "\"encoding\":\"base64\",\"compression\":\"gzip\","), "layers[0].data"},
        {minimalMap("\"AAAAAAAA\"", "\"encoding\":\"base64\",\"compression\":\"zstd\","), "layers[0].data"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.field);
        const ImportProblem p = failure(c.text);
        EXPECT_EQ(p.kind, "parse");
        EXPECT_EQ(p.field, c.field) << p.message;
        EXPECT_FALSE(p.message.empty());
    }
}

// Nesting is bounded: a group chain past the limit is a parse error, and a document that is one
// deep array only has to fail, not overflow the stack
TEST(TiledImporterTest, DeepNestingIsRefused) {
    std::string layers = R"({"type":"group","name":"G","layers":[]})";
    for (int i = 0; i < 200; i++) layers = R"({"type":"group","name":"G","layers":[)" + layers + "]}";
    const ImportProblem p = failure(R"({"type":"map","orientation":"orthogonal","width":1,"height":1,"tilewidth":16,"tileheight":16,"tilesets":[],"layers":[)" + layers + "]}");
    EXPECT_EQ(p.kind, "parse");
    EXPECT_NE(p.message.find("nest"), std::string::npos) << p.message;
    EXPECT_FALSE(p.field.empty());

    std::string deep(200000, '[');
    deep += std::string(200000, ']');
    EXPECT_EQ(failure(deep).kind, "parse");
}

// Malformed maps must never crash the importer: every fixture, truncated and with bytes flipped
// (the whole file parses as data, so anything can reach the mapper)
TEST(TiledImporterTest, CorruptFilesNeverCrash) {
    std::mt19937 random(12345);
    const char* files[] = {"features.tmj", "features.tmx", "ortho_zlib.tmx", "ortho_zstd.tmj", "ortho_gzip.tmj", "infinite_zlib.tmj", "wang.tmj", "hex_y_odd_h3.tmj", "collection.tmj"};
    int runs = 0;
    for (const char* file : files) {
        const std::string text = readFixture(file);
        for (size_t cut = 0; cut < text.size(); cut += std::max<size_t>(1, text.size() / 48)) {
            ImportPlan plan;
            std::vector<ImportProblem> problems;
            parseTiled(std::string_view(text).substr(0, cut), file, fixtureReader(), plan, problems);
            runs++;
        }
        for (int round = 0; round < 48; round++) {
            std::string corrupt = text;
            for (int i = 0; i < 16; i++) {
                std::uniform_int_distribution<size_t> position(0, corrupt.size() - 1);
                std::uniform_int_distribution<int> value(0, 255);
                corrupt[position(random)] = static_cast<char>(value(random));
            }
            ImportPlan plan;
            std::vector<ImportProblem> problems;
            parseTiled(corrupt, file, fixtureReader(), plan, problems);
            runs++;
        }
    }
    EXPECT_GT(runs, 400);
}
