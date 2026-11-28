// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// game.json's crash object: Player > Crash Handling of a prebuilt player, with the Gameplay
// Journal's size and engine events (docs/specs/shipped-game-diagnostics.md R41, AC22).

#include "player/PlayerConfig.h"

#include <gtest/gtest.h>

namespace PlayerConfigCrashTest_cpp {

using namespace jasperengine::player;

TEST(PlayerConfigCrash, JournalRoundTrips) {
    PlayerConfig game;
    game.name = "g";
    game.crashJournalBytes = 128 << 10;
    game.crashJournalEngineEvents = false;
    EXPECT_EQ(game.toJson()["crash"]["journalBytes"], 128 << 10);
    PlayerConfig back;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(game.toJson(), back, error)) << error;
    EXPECT_EQ(back.crashJournalBytes, 128 << 10);
    EXPECT_FALSE(back.crashJournalEngineEvents);
}

TEST(PlayerConfigCrash, OlderGameJsonGetsTheDefaultJournal) {
    PlayerConfig game;
    game.name = "g";
    nlohmann::json json = game.toJson();
    json["crash"].erase("journalBytes");
    json["crash"].erase("journalEngineEvents");
    PlayerConfig back;
    back.crashJournalBytes = 0;
    back.crashJournalEngineEvents = false;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(json, back, error)) << error;
    EXPECT_EQ(back.crashJournalBytes, 32 << 10);
    EXPECT_TRUE(back.crashJournalEngineEvents);
}

}  // namespace PlayerConfigCrashTest_cpp
