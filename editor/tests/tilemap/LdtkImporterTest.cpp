// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The LDtk importer (docs/specs/S50-grid-tilemap.md AC8, R45, D11): Tiles, AutoLayer and IntGrid
// layers, external levels, the skipped items and malformed files. Goldens are hand-derived.

#include "ImportPlanDump.h"

#include "tilemap/import/LdtkImporter.h"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::tilemapimport;
using namespace jasperengine::editor::tilemapimport::testing;

namespace {

    ImportPlan parseLdtkFixture(const std::string& name) {
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        const bool ok = parseLdtk(readFixture(name), name, fixtureReader(), plan, problems);
        EXPECT_TRUE(ok) << name << ": " << (problems.empty() ? std::string() : problems[0].field + ": " + problems[0].message);
        return plan;
    }

    ImportProblem ldtkFailure(const std::string& text) {
        ImportPlan plan;
        std::vector<ImportProblem> problems;
        const bool ok = parseLdtk(text, "bad.ldtk", FileReader(), plan, problems);
        EXPECT_FALSE(ok) << text;
        return problems.empty() ? ImportProblem() : problems[0];
    }

    std::string projectWithLevels(const std::string& levels, const std::string& defsExtra = "") {
        return R"({"defaultGridSize":16,"defs":{"layers":[],"tilesets":[)" + defsExtra + R"(]},"levels":)" + levels + "}";
    }

    std::string levelWith(const std::string& instance) {
        return R"([{"identifier":"L","worldX":0,"worldY":0,"pxWid":32,"pxHei":32,"layerInstances":[)" + instance + "]}]";
    }

}

TEST(LdtkImporterTest, TilesAutoLayerAndIntGridEqualGolden) {
    const ImportPlan plan = parseLdtkFixture("project.ldtk");
    const nlohmann::json actual = planJson(plan);
    const nlohmann::json expected = goldenJson("project.expected.json");
    EXPECT_EQ(actual, expected) << actual.dump(1);
    EXPECT_EQ(plan.format, "ldtk");
    ASSERT_EQ(plan.grids.size(), 1u);
    EXPECT_EQ(plan.grids[0].name, "Level_0");
}

TEST(LdtkImporterTest, ExternalLevelsEqualGolden) {
    const ImportPlan plan = parseLdtkFixture("external.ldtk");
    const nlohmann::json actual = planJson(plan);
    const nlohmann::json expected = goldenJson("external_ldtk.expected.json");
    EXPECT_EQ(actual, expected) << actual.dump(1);
}

TEST(LdtkImporterTest, TileIdsComeFromSrcPositions) {
    const std::string tileset = R"({"uid":1,"identifier":"T","relPath":"t.png","pxWid":40,"pxHei":40,"tileGridSize":16,"spacing":4,"padding":2})";
    const std::string instance = R"({"__identifier":"Tiles","__type":"Tiles","__gridSize":16,"__cWid":2,"__cHei":2,"__tilesetDefUid":1,
        "gridTiles":[{"px":[0,0],"src":[22,2]},{"px":[16,16],"src":[2,22]}]})";
    ImportPlan plan;
    std::vector<ImportProblem> problems;
    ASSERT_TRUE(parseLdtk(projectWithLevels(levelWith(instance), tileset), "p.ldtk", FileReader(), plan, problems)) << problems[0].message;
    // 2 columns; (22-2)/20 = column 1, row 0 -> id 1; (2-2)/20 = column 0, row 1 -> id 2
    ASSERT_EQ(plan.grids[0].layers[0].cells.size(), 2u);
    EXPECT_EQ(cellValueTile(plan.grids[0].layers[0].cells[0].value), 2);   // (1, 0) (px (16, 16), y up)
    EXPECT_EQ(cellValueTile(plan.grids[0].layers[0].cells[1].value), 1);   // (0, 1) (px (0, 0))
    EXPECT_EQ(plan.tilesets[0].data.sources[0].margin, 2);
    EXPECT_EQ(plan.tilesets[0].data.sources[0].spacing, 4);
}

TEST(LdtkImporterTest, MalformedProjectsGiveParseErrorsWithPaths) {
    {
        const ImportProblem p = ldtkFailure("{\n\"defs\": 3");
        EXPECT_EQ(p.kind, "parse");
        EXPECT_GE(p.line, 1);
    }
    const std::string tileset = R"({"uid":1,"identifier":"T","relPath":"t.png","pxWid":16,"pxHei":16,"tileGridSize":16,"spacing":0,"padding":0})";
    const struct {
        std::string text;
        const char* field;
    } cases[] = {
        {R"({"levels":[]})", "defs"},
        {projectWithLevels(R"(5)"), "levels"},
        {projectWithLevels(levelWith(R"({"__identifier":"L","__type":"Tiles","__gridSize":0,"__cWid":1,"__cHei":1})")), "levels[0].layerInstances[0].__gridSize"},
        {projectWithLevels(levelWith(R"({"__identifier":"L","__type":"Tiles","__gridSize":16,"__cWid":1,"__cHei":1,"__tilesetDefUid":9,"gridTiles":[]})")), "levels[0].layerInstances[0].__tilesetDefUid"},
        {projectWithLevels(levelWith(R"({"__identifier":"L","__type":"Tiles","__gridSize":16,"__cWid":1,"__cHei":1,"__tilesetDefUid":1,"gridTiles":[{"px":[0],"src":[0,0]}]})"), tileset), "levels[0].layerInstances[0].gridTiles[0].px"},
        {projectWithLevels(levelWith(R"({"__identifier":"L","__type":"IntGrid","__gridSize":16,"__cWid":2,"__cHei":2,"layerDefUid":3,"__tilesetDefUid":1,"intGridCsv":[1]})"), tileset), "levels[0].layerInstances[0].intGridCsv"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.field);
        const ImportProblem p = ldtkFailure(c.text);
        EXPECT_EQ(p.kind, "parse");
        EXPECT_EQ(p.field, c.field) << p.message;
    }
    // an IntGrid layer with auto tiles but no tileset: its generated tileset has no image
    {
        const std::string defs = R"({"defaultGridSize":16,"defs":{"layers":[{"uid":3,"identifier":"L","intGridValues":[{"value":1,"color":"#000000"}]}],"tilesets":[]},"levels":)";
        const std::string level = levelWith(R"({"__identifier":"L","__type":"IntGrid","__gridSize":16,"__cWid":1,"__cHei":1,"layerDefUid":3,"intGridCsv":[1],"autoLayerTiles":[{"px":[0,0],"src":[0,0]}]})");
        const ImportProblem p = ldtkFailure(defs + level + "}");
        EXPECT_EQ(p.kind, "parse");
        EXPECT_EQ(p.field, "levels[0].layerInstances[0].autoLayerTiles[0]") << p.message;
    }
    // an external level that cannot be read names its path field
    ImportPlan plan;
    std::vector<ImportProblem> problems;
    EXPECT_FALSE(parseLdtk(projectWithLevels(R"([{"identifier":"E","externalRelPath":"gone.ldtkl","layerInstances":null}])"), "p.ldtk", [](const std::string&, std::string&) { return false; }, plan, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].field, "levels[0].externalRelPath");
}

TEST(LdtkImporterTest, CorruptProjectsNeverCrash) {
    std::mt19937 random(777);
    int runs = 0;
    for (const char* file : {"project.ldtk", "external.ldtk", "external/Ext_0.ldtkl"}) {
        const std::string text = readFixture(file);
        for (size_t cut = 0; cut < text.size(); cut += std::max<size_t>(1, text.size() / 48)) {
            ImportPlan plan;
            std::vector<ImportProblem> problems;
            parseLdtk(std::string_view(text).substr(0, cut), file, fixtureReader(), plan, problems);
            runs++;
        }
        for (int round = 0; round < 64; round++) {
            std::string corrupt = text;
            for (int i = 0; i < 16; i++) {
                std::uniform_int_distribution<size_t> position(0, corrupt.size() - 1);
                std::uniform_int_distribution<int> value(0, 255);
                corrupt[position(random)] = static_cast<char>(value(random));
            }
            ImportPlan plan;
            std::vector<ImportProblem> problems;
            parseLdtk(corrupt, file, fixtureReader(), plan, problems);
            runs++;
        }
    }
    EXPECT_GT(runs, 300);
}
