// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Two editors sharing the global settings file merge their changes (docs/specs/S03-data-safety.md
// R11, AC7)

#include "AppSettings.h"
#include "support/TestTempDir.h"
#include "util/FileUtils.h"

#include <gtest/gtest.h>

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if !defined(_WIN32)
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>

    extern char** environ;
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

#if !defined(_WIN32)
namespace {
    // Set for process B: "<settings file>|<project>". B is this test binary started again with
    // this test filtered, as a second editor is another process. (A fork() child of this process,
    // which has job workers and the logger's drain thread, may only call async-signal-safe
    // functions: a lock another thread held at the fork stays held in it, as the logger's did in
    // AppSettings::applySettings under ASan, and the child waited for good.)
    constexpr const char* processBEnv = "JE_APPSETTINGS_TEST_PROCESS_B";
}

// A loads, B loads, B adds a recent project and saves, A changes the window width and saves: the
// file has both
TEST(AppSettings, MergeConcurrent) {
    if (const char* processB = std::getenv(processBEnv)) {
        const std::string spec = processB;
        const size_t bar = spec.find('|');
        bool saved = AppSettings::initializeWithFile(spec.substr(0, bar));
        if (saved) {
            AppSettings::addToRecentProjects(spec.substr(bar + 1), false);
            saved = AppSettings::saveSettings();
        }
        std::fflush(nullptr);
        ::_exit(saved ? 0 : 1);
    }

    TestTempDir dir("settings");
    const fs::path file = dir / "settings.yaml";
    const fs::path projectA = dir / "ProjectA";
    const fs::path projectB = dir / "ProjectB";
    fs::create_directories(projectA);
    fs::create_directories(projectB);
    TestTempDir::write(file, "recent_projects:\n  - " + projectA.string() + "\nwindow:\n  width: 1280\n  height: 720\n  maximized: false\n");

    ASSERT_TRUE(AppSettings::initializeWithFile(file));
    ASSERT_EQ(AppSettings::getWindowWidth(), 1280);

    // process B
    const std::string self = (editor::FileUtils::getExecutableDir() / "jasperengine-editor-tests").string();
    const std::string filter = "--gtest_filter=AppSettings.MergeConcurrent";
    const std::string bEnv = std::string(processBEnv) + "=" + file.string() + "|" + projectB.string();
    std::vector<char*> args = {const_cast<char*>(self.c_str()), const_cast<char*>(filter.c_str()), nullptr};
    std::vector<char*> env;
    for (char** entry = environ; *entry; entry++) env.push_back(*entry);
    env.push_back(const_cast<char*>(bEnv.c_str()));
    env.push_back(nullptr);
    pid_t child = 0;
    ASSERT_EQ(::posix_spawn(&child, self.c_str(), nullptr, nullptr, args.data(), env.data()), 0) << self;
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    // process A, which read the file before B wrote it
    AppSettings::setWindowWidth(1600);
    ASSERT_TRUE(AppSettings::saveSettings());

    const YAML::Node saved = YAML::LoadFile(file.string());
    EXPECT_EQ(saved["window"]["width"].as<int>(), 1600);
    std::vector<std::string> recent;
    // records since S27 R21: {path, name, last_opened, ...}
    for (const auto& entry : saved["recent_projects"]) recent.push_back(fs::path(entry["path"].as<std::string>()).string());
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
