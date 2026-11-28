// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project-test-runner R50, AC17: a Development build with Include Tests carries the test root as
// tests/ (without Golden/, with tests/index.json); a release build with the setting carries nothing
// and warns.

#include "test/GameTests.h"

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>

namespace ExportTestsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {
    void writeFile(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
}

TEST(ExportTests, IncludeTestsOnlyInDevelopmentBuilds) {
    TestTempDir dir("export-tests");
    const fs::path project = dir.path() / "Game";
    writeFile(project / "Tests" / "tests.yaml", "version: 1\nfixed_fps: 30\n");
    writeFile(project / "Tests" / "menu.test.lua", "Test.case('a', function(t) end)\n");
    writeFile(project / "Tests" / "shop" / "buy.test.lua", "Test.case('b', function(t) end)\n");
    writeFile(project / "Tests" / "Data" / "one_coin" / "progress.json", "{\"coins\": 1}");
    writeFile(project / "Tests" / "Golden" / "menu.test.lua__a" / "start.png", "png");

    // a release build: nothing, one warning
    const fs::path release = dir.path() / "release";
    std::string warning, error;
    ASSERT_TRUE(gametests::exportTests(project, false, true, release, warning, error)) << error;
    EXPECT_EQ(warning, gametests::INCLUDE_TESTS_RELEASE_WARNING);
    EXPECT_FALSE(fs::exists(release / "tests"));

    // the setting off: nothing, no warning
    ASSERT_TRUE(gametests::exportTests(project, true, false, release, warning, error)) << error;
    EXPECT_TRUE(warning.empty());
    EXPECT_FALSE(fs::exists(release / "tests"));

    // a Development build: tests/ without Golden/, and the index a player reads
    const fs::path development = dir.path() / "development";
    ASSERT_TRUE(gametests::exportTests(project, true, true, development, warning, error)) << error;
    EXPECT_TRUE(warning.empty()) << warning;
    EXPECT_TRUE(fs::is_regular_file(development / "tests" / "tests.yaml"));
    EXPECT_TRUE(fs::is_regular_file(development / "tests" / "shop" / "buy.test.lua"));
    EXPECT_TRUE(fs::is_regular_file(development / "tests" / "Data" / "one_coin" / "progress.json"));
    EXPECT_FALSE(fs::exists(development / "tests" / "Golden"));
    const gametest::Json index = gametest::Json::parse(editor::test::readText(development / "tests" / "index.json"));
    EXPECT_EQ(index["settings"]["fixed_fps"], 30);
    EXPECT_EQ(index["files"], (gametest::Json::array({"menu.test.lua", "shop/buy.test.lua"})));
}

}  // namespace ExportTestsTest_cpp
