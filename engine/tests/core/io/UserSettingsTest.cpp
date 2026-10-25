// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime UserSettings.xml is replaced atomically (docs/specs/S03-data-safety.md B4, AC21)

#include "UserSettings.h"
#include "Engine.h"
#include "System.h"
#include "io/AtomicFile.h"
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
            system->setUserDataPath(dir.path().string());
            system->fileSystemSyncs = 0;
        }
        void TearDown() override {
            io::setAtomicFileFaultInjector(nullptr);
            system->setUserDataPath("");
        }

        TestTempDir dir{"usersettings"};
        test::TestSystem* system = nullptr;
    };

}

TEST_F(UserSettingsTest, AtomicOnCrash){
    UserSettings::setStringForKey("player", "Ada");
    UserSettings::setIntegerForKey("level", 3);
    const fs::path file = dir / "UserSettings.xml";
    ASSERT_TRUE(fs::exists(file));
    const std::string saved = TestTempDir::read(file);
    EXPECT_GE(system->fileSystemSyncs, 2);

    // the process "dies" in the middle of the next save: the rename never happens
    io::setAtomicFileFaultInjector([](std::string_view step, const fs::path&) -> std::error_code {
        return step == "rename" ? std::make_error_code(std::errc::io_error) : std::error_code();
    });
    UserSettings::setIntegerForKey("level", 4);
    io::setAtomicFileFaultInjector(nullptr);

    EXPECT_EQ(TestTempDir::read(file), saved);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 3);
    EXPECT_EQ(UserSettings::getStringForKey("player"), "Ada");
    for (const auto& entry : fs::directory_iterator(dir.path())){
        EXPECT_FALSE(io::isTemporarySiblingName(entry.path().filename().string())) << entry.path();
    }

    UserSettings::setIntegerForKey("level", 5);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 5);
    UserSettings::removeKey("player");
    EXPECT_EQ(UserSettings::getStringForKey("player", "none"), "none");
}
