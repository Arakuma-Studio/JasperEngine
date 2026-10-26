// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The AdMob App ID check of the Project Settings, the MCP project settings and the export

#include <gtest/gtest.h>

#include "Project.h"

using namespace jasperengine::editor;

TEST(AdMobAppId, AcceptsAppIds) {
    EXPECT_TRUE(isAdMobAppIdValid("ca-app-pub-1234567890123456~1234567890"));
    EXPECT_TRUE(isAdMobAppIdValid(androidSampleAdMobAppId));
    EXPECT_TRUE(isAdMobAppIdValid(iosSampleAdMobAppId));
}

TEST(AdMobAppId, RejectsAdUnitIdsAndMalformedIds) {
    // an ad unit id, which is what a game passes to loadInterstitialAd
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-1234567890123456/1234567890"));
    EXPECT_FALSE(isAdMobAppIdValid(""));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-"));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-~123"));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-123~"));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-12a4~123"));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-123~45~6"));
    EXPECT_FALSE(isAdMobAppIdValid(" ca-app-pub-123~456"));
    EXPECT_FALSE(isAdMobAppIdValid("ca-app-pub-123~456 "));
    EXPECT_FALSE(isAdMobAppIdValid("pub-1234567890123456~1234567890"));
}
