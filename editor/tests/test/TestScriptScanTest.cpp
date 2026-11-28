// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project test runner's static listing and Play Data snapshots (docs/specs/project-test-runner.md
// AC1, AC7): cases are listed by a text scan that runs no project code; a snapshot session is a
// copy of Tests/Data/<name>/, gone when the session ends, the snapshot untouched.

#include "Project.h"
#include "command/CommandHandle.h"
#include "test/GameTests.h"
#include "test/PlayData.h"
#include "test/TestScan.h"

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>

namespace TestScriptScanTest_cpp {

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

TEST(TestScriptScan, ListsCasesWithoutRunningCode) {
    const std::string code = R"lua(-- a file whose top level fails when it runs
error("this file is never run by the listing")
Test.config{ scene = "Scenes/Menu.scene", tags = {"smoke"} }
--[[ Test.case("in a comment", function(t) end) ]]
Test.case("Start opens Level 1", function(t)
    local s = "Test.case('inside a string', function() end)"
end)
Test.case('edit case', {mode = "edit", tags = {"fast", "red"}}, function(t) end)
Test.case("skipped", {skip = "later", data = "one_coin", fixedFps = 30}, function(t) end)
Test.case(computedName(), function(t) end)
)lua";
    gametest::Settings settings;
    const gametest::ScannedFile file = gametest::TestScan::scanText(code, "ui/menu.test.lua", settings);
    ASSERT_EQ(file.cases.size(), 3u);
    EXPECT_EQ(file.cases[0].id, "ui/menu.test.lua#Start opens Level 1");
    EXPECT_EQ(file.cases[0].line, 5);
    EXPECT_EQ(file.cases[0].mode, gametest::Mode::Play);
    EXPECT_EQ(file.cases[0].scene, "Scenes/Menu.scene");
    EXPECT_EQ(file.cases[0].tags, std::vector<std::string>{"smoke"});
    EXPECT_EQ(file.cases[1].name, "edit case");
    EXPECT_EQ(file.cases[1].line, 8);
    EXPECT_EQ(file.cases[1].mode, gametest::Mode::Edit);
    EXPECT_EQ(file.cases[1].tags, (std::vector<std::string>{"smoke", "fast", "red"}));
    EXPECT_EQ(file.cases[2].skip, "later");
    EXPECT_EQ(file.cases[2].data, "one_coin");
    EXPECT_EQ(file.cases[2].fixedFps, 30);

    // 200 files in at most 50 ms (R3, R37)
    TestTempDir dir("test-script-scan");
    for (int i = 0; i < 200; i++) writeFile(dir.path() / ("dir" + std::to_string(i % 10)) / ("f" + std::to_string(i) + ".test.lua"), code);
    writeFile(dir.path() / "Golden" / "x.test.lua", code);
    const auto start = std::chrono::steady_clock::now();
    const std::vector<gametest::ScannedFile> files = gametest::TestScan::scan(dir.path(), settings);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(files.size(), 200u) << "Golden/ is not scanned";
    std::printf("[bench] TestScriptScan 200 files %.2f ms (budget 50)\n", ms);
    if (editor::test::perfSlack() > 0.0) {
        EXPECT_LE(ms, 50.0 * editor::test::perfSlack());
    }
}

TEST(TestScriptScan, DuplicateNamesAreMarked) {
    gametest::Settings settings;
    const gametest::ScannedFile file = gametest::TestScan::scanText("Test.case('a', f)\nTest.case('a', g)\nTest.case('b', h)\n", "d.test.lua", settings);
    ASSERT_EQ(file.cases.size(), 3u);
    EXPECT_TRUE(file.cases[0].duplicate);
    EXPECT_TRUE(file.cases[1].duplicate);
    EXPECT_FALSE(file.cases[2].duplicate);
}

TEST(PlayDataProfile, SnapshotSessionIsACopy) {
    TestTempDir dir("play-data-profile");
    const fs::path root = dir.path() / "Game";
    Project project;
    project.setSkipProjectLock(true);
    std::string error;
    ASSERT_TRUE(project.createProjectAt(root, "Game", &error)) << error;
    const fs::path top = project.getProjectPath();
    writeFile(top / "Tests" / "tests.yaml", "version: 1\n");
    writeFile(top / "Tests" / "Data" / "one_coin" / "progress.json", "{\"coins\": 1}");
    const fs::path rundata = project.getUserDataPath();
    EXPECT_EQ(gametests::listSnapshots(top), std::vector<std::string>{"one_coin"});

    gametests::PlayData& data = gametests::PlayData::get();
    ASSERT_TRUE(data.sessionStarting(&project, "one_coin", error)) << error;
    ASSERT_TRUE(data.active());
    const fs::path session = data.folder();
    EXPECT_NE(session, rundata);
    EXPECT_EQ(project.getUserDataPath(), session) << "data:// is the session folder";
    EXPECT_EQ(editor::test::readText(session / "progress.json"), "{\"coins\": 1}");
    writeFile(session / "progress.json", "{\"coins\": 2}");
    // no Play started: the next frame ends the session, its folder goes
    data.update(&project);
    EXPECT_FALSE(data.active());
    EXPECT_FALSE(fs::exists(session));
    EXPECT_EQ(project.getUserDataPath(), rundata);
    EXPECT_EQ(editor::test::readText(top / "Tests" / "Data" / "one_coin" / "progress.json"), "{\"coins\": 1}") << "the snapshot is untouched";

    // Clean is an empty folder; a name that is no snapshot is refused, naming the snapshots
    ASSERT_TRUE(data.sessionStarting(&project, "clean", error)) << error;
    EXPECT_TRUE(fs::is_empty(data.folder()));
    data.update(&project);
    EXPECT_FALSE(data.sessionStarting(&project, "two_coins", error));
    EXPECT_NE(error.find("one_coin"), std::string::npos) << error;
    for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
    project.releaseScenes();
}

}  // namespace TestScriptScanTest_cpp
