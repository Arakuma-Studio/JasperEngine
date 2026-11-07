// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// game.json's nativePlatformOnly (mobile-shipping spec 6.8): the prebuilt player learns which
// native plugins have code for other platforms only, as a Compile build's generated init does.

#include "player/PlayerConfig.h"

#include <gtest/gtest.h>

using namespace jasperengine::player;

TEST(PlayerConfigNative, PlatformOnlyPluginsRoundTrip) {
    PlayerConfig game;
    game.name = "g";
    EXPECT_FALSE(game.toJson().contains("nativePlatformOnly")) << "omitted when empty";
    game.nativePlatformOnly = {{"Ads", "Java/Kotlin plugins run only on Android"}};
    PlayerConfig back;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(game.toJson(), back, error)) << error;
    EXPECT_EQ(back.nativePlatformOnly, game.nativePlatformOnly);
}
