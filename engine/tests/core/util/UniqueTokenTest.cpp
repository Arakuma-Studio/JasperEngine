// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/UniqueToken.h"

#include <gtest/gtest.h>

#include <cctype>
#include <set>
#include <string>

using namespace jasperengine;

TEST(UniqueToken, GetNeverRepeats){
    std::set<std::string> tokens;
    for (int i = 0; i < 1000; i++)
        tokens.insert(UniqueToken::get());
    EXPECT_EQ(tokens.size(), 1000u);
}

TEST(UniqueToken, TenRandomCharactersThenACounter){
    const std::string first = UniqueToken::get();
    const std::string second = UniqueToken::get();
    ASSERT_GT(first.size(), 10u);
    for (size_t i = 0; i < 10; i++)
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(first[i]))) << first;
    EXPECT_EQ(std::stoi(second.substr(10)), std::stoi(first.substr(10)) + 1);
}
