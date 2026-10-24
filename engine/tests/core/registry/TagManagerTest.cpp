// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "registry/TagManager.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    // the tags and layer names are process-wide: every test starts and ends with the built-in ones
    class TagManagerTest : public ::testing::Test {
    protected:
        void SetUp() override{ TagManager::reset(); }
        void TearDown() override{ TagManager::reset(); }
    };
}

TEST_F(TagManagerTest, BuiltInTagsAreUnitys){
    EXPECT_EQ(TagManager::getBuiltinTags(), (std::vector<std::string>{"Untagged", "Respawn", "Finish", "EditorOnly", "MainCamera", "Player", "GameController"}));
    EXPECT_TRUE(TagManager::isTagDefined("MainCamera"));
    EXPECT_FALSE(TagManager::isTagDefined("Enemy"));
}

TEST_F(TagManagerTest, UserTagsAreTrimmedDeduplicatedAndAfterTheBuiltIns){
    LogCapture log;
    TagManager::setTags({" Enemy ", "Coin", "Enemy", "", "Player"});
    EXPECT_EQ(TagManager::getTags(), (std::vector<std::string>{"Enemy", "Coin"}));
    EXPECT_EQ(log.count(D_LOG_WARN, "skipped"), 3u);
    const std::vector<std::string> all = TagManager::getAllTags();
    ASSERT_EQ(all.size(), 9u);
    EXPECT_EQ(all[7], "Enemy");
    EXPECT_TRUE(TagManager::isTagDefined("Coin"));
}

TEST_F(TagManagerTest, BuiltInLayers){
    EXPECT_TRUE(TagManager::isBuiltinLayer(0));
    EXPECT_TRUE(TagManager::isBuiltinLayer(5));
    EXPECT_FALSE(TagManager::isBuiltinLayer(3));
    EXPECT_FALSE(TagManager::isBuiltinLayer(32));
    EXPECT_STREQ(TagManager::getBuiltinLayerName(2), "Ignore Raycast");
    EXPECT_STREQ(TagManager::getBuiltinLayerName(6), "");
}

TEST_F(TagManagerTest, LayerNamesAndLookups){
    EXPECT_TRUE(TagManager::setLayerName(8, "  Enemies "));
    EXPECT_EQ(TagManager::layerToName(8), "Enemies");
    EXPECT_EQ(TagManager::nameToLayer("Enemies"), 8);
    EXPECT_EQ(TagManager::nameToLayer("Water"), 4);
    EXPECT_EQ(TagManager::nameToLayer("Nothing"), -1);
    EXPECT_EQ(TagManager::layerToName(40), "");
}

TEST_F(TagManagerTest, LayerNamesRefuseBuiltInsDuplicatesAndBadIndices){
    LogCapture log;
    EXPECT_FALSE(TagManager::setLayerName(4, "Lava"));
    EXPECT_FALSE(TagManager::setLayerName(33, "Far"));
    EXPECT_TRUE(TagManager::setLayerName(9, "Props"));
    EXPECT_FALSE(TagManager::setLayerName(10, "Props"));
    EXPECT_EQ(log.errors().size(), 3u);
    EXPECT_TRUE(TagManager::setLayerName(9, ""));
    EXPECT_EQ(TagManager::nameToLayer("Props"), -1);
}

TEST_F(TagManagerTest, SetLayerNamesReplacesEveryUserLayer){
    TagManager::setLayerName(20, "Old");
    std::vector<std::string> names(MAX_LAYERS);
    names[3] = "Three";
    names[4] = "NotWater";
    TagManager::setLayerNames(names);
    EXPECT_EQ(TagManager::layerToName(3), "Three");
    EXPECT_EQ(TagManager::layerToName(4), "Water");
    EXPECT_EQ(TagManager::layerToName(20), "");
}

TEST_F(TagManagerTest, GetMaskHasABitPerKnownName){
    TagManager::setLayerName(7, "Seven");
    EXPECT_EQ(TagManager::getMask({"Default", "Seven", "Unknown"}), (1u << 0) | (1u << 7));
    EXPECT_EQ(LayerMask::getMask({"UI"}), 1u << 5);
    EXPECT_EQ(LAYER_MASK_DEFAULT_RAYCAST, ~(1u << 2));
}
