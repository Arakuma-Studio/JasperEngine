// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime UserSettings.xml is replaced atomically (docs/specs/S03-data-safety.md B4, AC21)

#include "UserSettings.h"
#include "Engine.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "io/UserSettingsStore.h"
#include "support/TestTempDir.h"
#include "TestSystem.h"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    // The test binary's System (TestMain started the engine with it), its user data directory
    // pointed at the test's own directory
    class UserSettingsTest: public ::testing::Test {
    protected:
        void SetUp() override {
            system = &test::TestSystem::get();
            // the store reads its file (data://UserSettings.xml) again, in this directory
            UserSettingsStore::instance().unload();
            system->setUserDataPath(dir.path().string());
            system->fileSystemSyncs = 0;
        }
        void TearDown() override {
            io::setAtomicFileFaultInjector(nullptr);
            UserSettingsStore::instance().unload();
            system->setUserDataPath("");
        }

        TestTempDir dir{"usersettings"};
        test::TestSystem* system = nullptr;
    };

}

// upstream's write-behind store (script data): a change is in memory at once and on the disk at
// the next save (UserSettings::save, a second later at the latest, app pause, shutdown); each
// save is S03's atomic write
TEST_F(UserSettingsTest, AtomicOnCrash){
    UserSettings::setStringForKey("player", "Ada");
    UserSettings::setIntegerForKey("level", 3);
    UserSettings::save();
    const fs::path file = dir / "UserSettings.xml";
    ASSERT_TRUE(fs::exists(file));
    const std::string saved = TestTempDir::read(file);
    EXPECT_GE(system->fileSystemSyncs, 1);

    // the process "dies" in the middle of the next save: the rename never happens
    io::setAtomicFileFaultInjector([](std::string_view step, const fs::path&) -> std::error_code {
        return step == "rename" ? std::make_error_code(std::errc::io_error) : std::error_code();
    });
    UserSettings::setIntegerForKey("level", 4);
    UserSettings::save();
    io::setAtomicFileFaultInjector(nullptr);

    // the file is the one of before, whole; the change waits for the next save
    EXPECT_EQ(TestTempDir::read(file), saved);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 4);
    EXPECT_EQ(UserSettings::getStringForKey("player"), "Ada");
    for (const auto& entry : fs::directory_iterator(dir.path())){
        EXPECT_FALSE(io::isTemporarySiblingName(entry.path().filename().string())) << entry.path();
    }
    UserSettings::save();
    EXPECT_NE(TestTempDir::read(file).find("4"), std::string::npos);

    UserSettings::setIntegerForKey("level", 5);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 5);
    UserSettings::removeKey("player");
    EXPECT_EQ(UserSettings::getStringForKey("player", "none"), "none");
}
