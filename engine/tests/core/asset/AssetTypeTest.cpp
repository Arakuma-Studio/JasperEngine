// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "asset/AssetType.h"

#include <set>
#include <string>

using namespace jasperengine;

namespace {

    class AssetTypeTest : public ::testing::Test {};

}

// every value up to the last has a name that reads back, so an appended type cannot be left out
// of assetTypeFromName's loop (its bound once stopped at VertexAnimation)
TEST_F(AssetTypeTest, NamesRoundTripEveryValue) {
    std::set<std::string> names;
    for (uint8_t value = 1; value <= static_cast<uint8_t>(LAST_ASSET_TYPE); value++) {
        // 24 to 27 are reserved for the pixel art program (docs/specs/S41-S52), not yet assigned
        if (value >= 24 && value <= 27) {
            continue;
        }
        const AssetType type = static_cast<AssetType>(value);
        const std::string name = assetTypeName(type);
        EXPECT_NE(name, "unknown") << "value " << (int)value << " has no name";
        EXPECT_TRUE(names.insert(name).second) << name << " is used twice";
        EXPECT_EQ(assetTypeFromName(name), type) << name;
    }
    EXPECT_EQ(assetTypeName(static_cast<AssetType>(static_cast<uint8_t>(LAST_ASSET_TYPE) + 1)), std::string("unknown"))
        << "LAST_ASSET_TYPE is the last value";
    EXPECT_EQ(assetTypeFromName("title_art"), AssetType::TitleArt);
    EXPECT_EQ(assetTypeFromName("sound_event"), AssetType::SoundEvent);
    EXPECT_EQ(static_cast<int>(AssetType::TitleArt), 29) << "the numbers are a file format: append only";
    EXPECT_EQ(assetTypeFromName("nothing"), AssetType::Unknown);
}
