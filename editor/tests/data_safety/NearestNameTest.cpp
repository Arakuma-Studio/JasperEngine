// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/NearestName.h"

#include <gtest/gtest.h>

#include <vector>

namespace NearestNameTest_cpp {

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

TEST(NearestName, IdentifierFoldsCaseAndUnderscores){
    const std::vector<std::string> keys = {"max_results", "max_doc_results", "include_docs", "with_components"};
    // camelCase for snake_case: four edits, past NearestName's two
    EXPECT_EQ(NearestName("maxDocResults", keys), "");
    EXPECT_EQ(NearestIdentifier("maxDocResults", keys), "max_doc_results");
    // a missing or an extra '_'
    EXPECT_EQ(NearestIdentifier("withcomponents", keys), "with_components");
    EXPECT_EQ(NearestIdentifier("include__docs", keys), "include_docs");
    // a plain typo, and a typo on top of a case confusion
    EXPECT_EQ(NearestIdentifier("max_resluts", keys), "max_results");
    EXPECT_EQ(NearestIdentifier("maxDocResluts", keys), "max_doc_results");
    EXPECT_EQ(NearestIdentifier("position", keys), "");
    EXPECT_EQ(NearestIdentifierSuffix("includeDocs", keys), " (did you mean 'include_docs'?)");
    EXPECT_EQ(NearestIdentifierSuffix("zzz", keys), "");
}

}  // namespace NearestNameTest_cpp
