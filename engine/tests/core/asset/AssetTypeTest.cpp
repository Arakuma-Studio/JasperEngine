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
    for (uint8_t value = 1; value <= kLastAssetType; value++) {
        // 27 is reserved for the pixel art program's Tileset (docs/specs/S41-S52), not yet assigned
        if (value == 27) {
            continue;
        }
        const AssetType type = static_cast<AssetType>(value);
        const std::string name = assetTypeName(type);
        EXPECT_NE(name, "unknown") << "value " << (int)value << " has no name";
        EXPECT_TRUE(names.insert(name).second) << name << " is used twice";
        EXPECT_EQ(assetTypeFromName(name), type) << name;
    }
    EXPECT_EQ(assetTypeName(static_cast<AssetType>(kLastAssetType + 1)), std::string("unknown"))
        << "kLastAssetType is the last value";
    EXPECT_EQ(assetTypeFromName("title_art"), AssetType::TitleArt);
    EXPECT_EQ(assetTypeFromName("sound_event"), AssetType::SoundEvent);
    EXPECT_EQ(static_cast<int>(AssetType::TitleArt), 34) << "the numbers are a file format: append only";
    EXPECT_EQ(static_cast<int>(AssetType::Mesh), 35) << "the numbers are a file format: append only";
    EXPECT_EQ(assetTypeFromName("mesh"), AssetType::Mesh);
    EXPECT_EQ(assetTypeFromName("ies_profile"), AssetType::IesProfile);
    EXPECT_EQ(static_cast<int>(AssetType::IesProfile), 36) << "the numbers are a file format: append only";
    EXPECT_EQ(static_cast<int>(AssetType::StringTable), 37) << "the numbers are a file format: append only";
    EXPECT_EQ(assetTypeFromName("stringtable"), AssetType::StringTable);
    EXPECT_EQ(static_cast<int>(AssetType::Dialogue), 38) << "the numbers are a file format: append only";
    EXPECT_EQ(assetTypeFromName("dialogue"), AssetType::Dialogue);
    EXPECT_EQ(assetTypeFromName("data_asset"), AssetType::DataAsset);
    EXPECT_EQ(static_cast<int>(AssetType::DataAsset), 39) << "the numbers are a file format: append only";
    EXPECT_EQ(static_cast<int>(AssetType::VertexColors), 40) << "the numbers are a file format: append only";
    EXPECT_EQ(static_cast<int>(AssetType::FogProfile), 41) << "the numbers are a file format: append only";
    EXPECT_EQ(assetTypeFromName("fog_profile"), AssetType::FogProfile);
    EXPECT_EQ(assetTypeFromName("nothing"), AssetType::Unknown);
}
