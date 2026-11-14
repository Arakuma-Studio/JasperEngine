// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Development Build's left-out assets (docs/specs/S98-editor-tooling-gaps.md REQ-10): a file on
// the manifest's list that does not open logs which setting left it out, once, instead of only a
// plain "not found"; one not on the list logs nothing more.

#include <gtest/gtest.h>

#include "LogCapture.h"

#include "io/File.h"
#include "io/FileData.h"
#include "io/StrippedAssets.h"
#include "player/ContentManifest.h"
#include "player/GameRunner.h"
#include "player/PlayerConfig.h"
#include "serialization/CookedScene.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;

namespace {

    struct StrippedListScope {
        explicit StrippedListScope(std::map<std::string, std::string> paths) { StrippedAssets::set(std::move(paths)); }
        ~StrippedListScope() { StrippedAssets::set({}); }
    };

}

TEST(StrippedAssets, AMissingStrippedFileNamesTheSettingOnce) {
    StrippedListScope list({{"stripping-test/b.png", "unreferenced"}, {"stripping-test/dev.png", "excluded: assets/dev/**"}});
    test::LogCapture log;
    File file;
    EXPECT_EQ(file.open("stripping-test/b.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(file.open("asset://stripping-test/b.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "stripping-test/b.png was stripped from the build (Asset Stripping: Referenced Only): add it to Always Include"), 1u);
    EXPECT_EQ(file.open("stripping-test/dev.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "stripping-test/dev.png was left out of the build (Exclude From Build: assets/dev/**)"), 1u);
    EXPECT_EQ(file.open("stripping-test/other.png"), (unsigned int)FileErrors::FILE_NOT_FOUND);
    EXPECT_EQ(log.count(D_LOG_WARN, "other.png"), 0u);
}

TEST(StrippedAssets, WithoutAListNothingIsExplained) {
    StrippedAssets::set({});
    EXPECT_TRUE(StrippedAssets::empty());
    EXPECT_FALSE(StrippedAssets::explainMissing("stripping-test/b.png"));
}

// A Compile build reads game.json through player::readGameConfig (its generated main.cpp): the
// content-manifest.json beside it gives the list, as the prebuilt player's own check does
TEST(StrippedAssets, ACompileBuildReadsTheListBesideItsGameJson) {
    StrippedAssets::set({});
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "jasper-stripped-compile-test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "game.json") << player::PlayerConfig().toJson().dump(2);
        player::ContentManifest manifest;
        manifest.engineVersion = "v1.0.0";
        manifest.contentFormatVersion = cook::contentFormatVersion();
        manifest.method = "compile";
        manifest.stripped["stripping-test/b.png"] = "unreferenced";
        std::ofstream(dir / "content-manifest.json") << manifest.toJson().dump(2);
    }
    player::PlayerConfig game;
    std::string error;
    EXPECT_TRUE(player::readGameConfig(dir.generic_string() + "/", game, error)) << error;
    EXPECT_FALSE(StrippedAssets::empty());
    EXPECT_TRUE(StrippedAssets::explainMissing("stripping-test/b.png"));
    StrippedAssets::set({});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
