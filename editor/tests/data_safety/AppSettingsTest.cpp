// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Two editors sharing the global settings file merge their changes (docs/specs/S03-data-safety.md
// R11, AC7)

#include "AppSettings.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include "yaml-cpp/yaml.h"

#if !defined(_WIN32)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

#if !defined(_WIN32)
// A loads, B loads (the fork), B adds a recent project and saves, A changes the window width and
// saves: the file has both
TEST(AppSettings, MergeConcurrent) {
    TestTempDir dir("settings");
    const fs::path file = dir / "settings.yaml";
    const fs::path projectA = dir / "ProjectA";
    const fs::path projectB = dir / "ProjectB";
    fs::create_directories(projectA);
    fs::create_directories(projectB);
    TestTempDir::write(file, "recent_projects:\n  - " + projectA.string() + "\nwindow:\n  width: 1280\n  height: 720\n  maximized: false\n");

    ASSERT_TRUE(AppSettings::initializeWithFile(file));
    ASSERT_EQ(AppSettings::getWindowWidth(), 1280);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // process B
        AppSettings::addToRecentProjects(projectB, false);
        ::_exit(AppSettings::saveSettings() ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    // process A, which read the file before B wrote it
    AppSettings::setWindowWidth(1600);
    ASSERT_TRUE(AppSettings::saveSettings());

    const YAML::Node saved = YAML::LoadFile(file.string());
    EXPECT_EQ(saved["window"]["width"].as<int>(), 1600);
    std::vector<std::string> recent;
    for (const auto& entry : saved["recent_projects"]) recent.push_back(entry.as<std::string>());
    EXPECT_NE(std::find(recent.begin(), recent.end(), projectA.string()), recent.end());
    EXPECT_NE(std::find(recent.begin(), recent.end(), projectB.string()), recent.end());
    // and A's view now has B's project too
    const auto inMemory = AppSettings::getRecentProjects();
    EXPECT_NE(std::find(inMemory.begin(), inMemory.end(), projectB), inMemory.end());
}
#endif

TEST(AppSettings, CorruptFileIsSetAside) {
    TestTempDir dir("settings");
    const fs::path file = dir / "settings.yaml";
    TestTempDir::write(file, "window: [unclosed\n  : :\n");
    AppSettings::initializeWithFile(file);
    bool asideFound = false;
    for (const auto& entry : fs::directory_iterator(dir.path())) {
        if (entry.path().filename().string().rfind("settings.yaml.corrupt-", 0) == 0) {
            asideFound = true;
            EXPECT_EQ(TestTempDir::read(entry.path()), "window: [unclosed\n  : :\n");
        }
    }
    EXPECT_TRUE(asideFound);
    // the next save starts from defaults instead of writing over the unreadable file
    EXPECT_TRUE(AppSettings::saveSettings());
    EXPECT_NO_THROW(YAML::LoadFile(file.string()));
}

TEST(AppSettings, AutosaveIntervalClamped) {
    AppSettings::setAutosaveIntervalSeconds(5);
    EXPECT_EQ(AppSettings::getAutosaveIntervalSeconds(), AppSettings::minAutosaveInterval);
    AppSettings::setAutosaveIntervalSeconds(1000);
    EXPECT_EQ(AppSettings::getAutosaveIntervalSeconds(), AppSettings::maxAutosaveInterval);
    AppSettings::setAutosaveIntervalSeconds(AppSettings::defaultAutosaveInterval);
}
