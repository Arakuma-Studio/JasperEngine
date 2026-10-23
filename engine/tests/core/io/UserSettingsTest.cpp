// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime UserSettings.xml is replaced atomically (docs/specs/S03-data-safety.md B4, AC21)

#include "UserSettings.h"
#include "Engine.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::test::TestTempDir;

namespace {

    // The platform System of a game, with the user data directory a test chooses
    class UserDataSystem: public System {
    public:
        int getScreenWidth() override { return 1; }
        int getScreenHeight() override { return 1; }
        std::string getUserDataPath() override { return dir.string(); }
        bool syncFileSystem() override { syncs++; return true; }

        fs::path dir;
        int syncs = 0;
    };

    // Owned by the engine for the rest of the process, like a platform's System
    UserDataSystem& engineSystem(){
        static UserDataSystem* system = []{
            auto* created = new UserDataSystem();
            char name[] = "jasperengine-core-tests";
            char* argv[] = {name, nullptr};
            Engine::systemInit(1, argv, created);
            return created;
        }();
        return *system;
    }

    class UserSettingsTest: public ::testing::Test {
    protected:
        void SetUp() override {
            system = &engineSystem();
            system->dir = dir.path();
            system->syncs = 0;
        }
        void TearDown() override {
            io::setAtomicFileFaultInjector(nullptr);
        }

        TestTempDir dir{"usersettings"};
        UserDataSystem* system = nullptr;
    };

}

TEST_F(UserSettingsTest, AtomicOnCrash){
    UserSettings::setStringForKey("player", "Ada");
    UserSettings::setIntegerForKey("level", 3);
    const fs::path file = dir / "UserSettings.xml";
    ASSERT_TRUE(fs::exists(file));
    const std::string saved = TestTempDir::read(file);
    EXPECT_GE(system->syncs, 2);

    // the process "dies" in the middle of the next save: the rename never happens
    io::setAtomicFileFaultInjector([](std::string_view step, const fs::path&) -> std::error_code {
        return step == "rename" ? std::make_error_code(std::errc::io_error) : std::error_code();
    });
    UserSettings::setIntegerForKey("level", 4);
    io::setAtomicFileFaultInjector(nullptr);

    EXPECT_EQ(TestTempDir::read(file), saved);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 3);
    EXPECT_EQ(UserSettings::getStringForKey("player"), "Ada");
    for (const auto& entry : fs::directory_iterator(dir.path()))
        EXPECT_FALSE(io::isTemporarySiblingName(entry.path().filename().string())) << entry.path();

    UserSettings::setIntegerForKey("level", 5);
    EXPECT_EQ(UserSettings::getIntegerForKey("level"), 5);
    UserSettings::removeKey("player");
    EXPECT_EQ(UserSettings::getStringForKey("player", "none"), "none");
}
