// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/NearestName.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

TEST(NearestName, SuggestsWithinTwoEdits){
    const std::vector<std::string> lights = {"directional", "point", "spot"};
    EXPECT_EQ(NearestName("spto", lights), "spot");
    EXPECT_EQ(NearestName("area", lights), "");
    EXPECT_EQ(NearestName("pointt", lights), "point");
    EXPECT_EQ(NearestNameSuffix("Ligth", std::vector<std::string>{"Light", "Sprite"}), " (did you mean 'Light'?)");
    EXPECT_EQ(LevenshteinCapped("kitten", "sitting", 5), 3u);
    EXPECT_EQ(LevenshteinCapped("kitten", "sitting", 1), 2u);
}
