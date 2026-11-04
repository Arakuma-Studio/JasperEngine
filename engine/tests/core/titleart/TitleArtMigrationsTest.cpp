// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "TestPaths.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtMigrations.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    class TitleArtMigrationsTest : public ::testing::Test {};

    // test-only steps (never in the product): v1 -> v2 renames `rims` to `outlines`, v2 -> v3
    // fills `glint` with the old behaviour (off)
    void renameRims(Json& root, MigrationLog& log) {
        if (!root.contains("layers")) return;
        for (Json& layer : root["layers"]) {
            if (layer.contains("rims")) {
                layer["outlines"] = layer["rims"];
                layer.erase("rims");
                log.note("rims moved to outlines");
            }
        }
    }

    void fillGlint(Json& root, MigrationLog&) {
        if (!root.contains("layers")) return;
        for (Json& layer : root["layers"]) {
            if (!layer.contains("glint")) layer["glint"] = false;
        }
    }

    Json sample() {
        return Json{{"format", TITLE_ART_FORMAT}, {"version", 1}, {"layers", Json::array({Json{{"rims", Json::array({1, 2})}}})}};
    }

}

TEST_F(TitleArtMigrationsTest, ChainRunsInOrder) {
    TitleJsonMigrations table("test format");
    ASSERT_TRUE(table.add({1, 2, "rename_rims", &renameRims}));
    ASSERT_TRUE(table.add({2, 3, "fill_glint", &fillGlint}));
    EXPECT_EQ(table.latestVersion(), 3);
    Json root = sample();
    MigrationLog log;
    std::string error;
    ASSERT_TRUE(table.run(root, 1, 3, log, &error)) << error;
    EXPECT_EQ(root["version"], 3);
    EXPECT_EQ(root["layers"][0]["outlines"], Json::array({1, 2}));
    EXPECT_FALSE(root["layers"][0].contains("rims"));
    EXPECT_EQ(root["layers"][0]["glint"], false);
    EXPECT_EQ(log.applied, (std::vector<std::string>{"v1->v2 rename_rims", "v2->v3 fill_glint"}));
    EXPECT_EQ(log.notes, (std::vector<std::string>{"rims moved to outlines"}));
    // from the middle of the chain
    Json v2 = Json{{"version", 2}, {"layers", Json::array({Json::object()})}};
    MigrationLog log2;
    ASSERT_TRUE(table.run(v2, 2, 3, log2, &error));
    EXPECT_EQ(log2.applied.size(), 1u);
}

TEST_F(TitleArtMigrationsTest, OutOfOrderStepsAreRefused) {
    TitleJsonMigrations table("test format");
    EXPECT_FALSE(table.add({2, 3, "skips_v1", &fillGlint})) << "the chain starts at the first version";
    EXPECT_TRUE(table.add({1, 2, "rename_rims", &renameRims}));
    EXPECT_FALSE(table.add({1, 2, "again", &renameRims})) << "a step continues the chain";
    EXPECT_FALSE(table.add({2, 4, "jumps", &fillGlint})) << "one version per step";
    EXPECT_FALSE(table.add({2, 3, "no_function", nullptr}));
    EXPECT_EQ(table.steps().size(), 1u);
    Json root = sample();
    MigrationLog log;
    std::string error;
    EXPECT_FALSE(table.run(root, 1, 3, log, &error)) << "no step reaches version 3";
    EXPECT_NE(error.find("version 2 to 3"), std::string::npos) << error;
    EXPECT_FALSE(table.run(root, 0, 2, log, &error)) << "older than the table";
}

TEST_F(TitleArtMigrationsTest, MigratingTwiceEqualsOnce) {
    for (auto step : {&renameRims, &fillGlint}) {
        Json once = sample();
        MigrationLog log;
        step(once, log);
        Json twice = once;
        step(twice, log);
        EXPECT_EQ(once, twice);
    }
}

TEST_F(TitleArtMigrationsTest, NewerVersionIsRefused) {
    const TitleArtParseResult parsed = parseTitleArt("{\"format\": \"jasperengine.titleart\", \"version\": " +
                                                     std::to_string(TITLE_ART_VERSION + 1) + "}");
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error.code, "version_newer");
}

TEST_F(TitleArtMigrationsTest, TheTitleArtTableIsTheCurrentVersion) {
    EXPECT_EQ(titleArtMigrations().latestVersion(), TITLE_ART_VERSION);
    EXPECT_EQ(titleArtMigrations().format(), TITLE_ART_FORMAT);
}

TEST_F(TitleArtMigrationsTest, EveryFixtureLoads) {
    const fs::path fixtures = fs::path(test::testPaths().sourceDir) / "titleart" / "fixtures";
    size_t loaded = 0;
    for (const auto& versionDir : fs::directory_iterator(fixtures)) {
        if (!versionDir.is_directory()) continue;
        for (const auto& entry : fs::directory_iterator(versionDir.path())) {
            if (entry.path().extension() != TITLE_ART_EXTENSION) continue;
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const TitleArtParseResult parsed = parseTitleArt(text);
            EXPECT_TRUE(parsed.ok) << entry.path() << ": " << parsed.error.toString();
            EXPECT_TRUE(parsed.warnings.empty()) << entry.path() << ": " << (parsed.warnings.empty() ? "" : parsed.warnings[0].toString());
            EXPECT_EQ(parsed.doc.root["version"], TITLE_ART_VERSION) << "migrated to the current version";
            loaded++;
        }
    }
    EXPECT_GE(loaded, 3u);
}
